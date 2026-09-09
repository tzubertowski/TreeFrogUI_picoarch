/* sf3000-hwdisp implementation. dlopen's driver.so, calls video_driver_*.
 *
 * Driver scales src dims → panel (854x480) via HCGE DMA with bilinear filter.
 * Two filter modes:
 *   - HW (default): pass src as-is, driver scales (bilinear).
 *   - Nearest: SW nearest-upscale src to driver native (1280x720) framing,
 *     so driver doesn't scale further → pixels stay sharp.
 *
 * Aspect-pad: when target aspect set, source is centered horizontally with
 * black pillar-bars so driver's stretch becomes uniform. */

#include "hwdisp.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <link.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
extern void dbg_log(const char *fmt, ...);
#define DBG(...) dbg_log(__VA_ARGS__)

#ifndef HW_NATIVE_W
#define HW_NATIVE_W 1280
#endif
#ifndef HW_NATIVE_H
#define HW_NATIVE_H  720
#endif
#define HW_W   HW_NATIVE_W
#define HW_H   HW_NATIVE_H
#define HW_PITCH (HW_W * 2)
#define HW_BUFSZ (HW_W * HW_H * 2)

static void   *g_handle = NULL;
static int     g_active = 0;
static int     g_native_game_surface = 0;

typedef int  (*fn_init_t)(void);
typedef void (*fn_deinit_t)(void);
typedef int  (*fn_disp_t)(void *src, int w, int h, int pitch);
typedef void (*fn_aspect_t)(int fullscreen);
typedef void (*fn_enhance_t)(int p0, int p1, int p2, int p3, int p4);
typedef void (*fn_setting_t)(const void *setting);
typedef int  (*fn_setmode_t)(int flags, int mode);

static fn_init_t   p_init   = NULL;
static fn_deinit_t p_deinit = NULL;
static fn_disp_t   p_disp   = NULL;
/* SF3000 driver exports g_render: a pointer to the live fb render context.
 * fbdev_video_aspect_ratio() updates only its global default; fb_paint_task
 * reads the copied fullscreen bit at context+16. */
static void       **p_g_render = NULL;
/* Driver's misleadingly named aspect toggle (fbdev_video_aspect_ratio).
 * Reverse engineering the SF3000 driver (driver sha256 90727162..., function
 * 0x5ba4) shows that it stores `argument == 0` into ge_is_full_screen:
 *
 *     argument 0 -> stored fullscreen 1 -> stretch/fill
 *     argument 1 -> stored fullscreen 0 -> preserve source aspect / fit
 *
 * Its "is_full_screen:%d" printf logs the ARGUMENT, not the stored value. Do
 * not infer the internal state from that driver message. */
static fn_aspect_t p_aspect = NULL;
static fn_setting_t p_setting = NULL;
typedef void (*fn_hcge_set_state_t)(void *, void *, unsigned int);
static fn_hcge_set_state_t g_hcge_set_state_orig = NULL;
static unsigned int g_hcge_hook_calls = 0;
static fn_setmode_t p_setmode = NULL;
static int         g_fs_state = -1;   /* last fill/aspect value pushed */
static int         g_fs_num = -1, g_fs_den = -1; /* last ratio programmed */

/* Display-controller enhance/sharpness probe (exported fbdev_set_enhance):
 * 5 params, first four 0-100, fifth 0-10 (a mode-like selector). Sweep them
 * on-device by editing /mnt/sdcard/enhance.txt (5 ints) — read at init, so a
 * game relaunch applies new values with no rebuild. Testing whether any combo
 * sharpens the GE's bilinear scale toward nearest. */
static fn_enhance_t p_enhance = NULL;
static int          g_sharpen = -1;   /* last sharpness pushed (0-10); -1 = unset */
static int          g_panel_scale;
static int          g_aspect_num, g_aspect_den;

/* Plaintext stock driver setting layout, recovered from its format string and
 * video_driver_setting() stores: colour format, screen width/height, image
 * width/height and image pitch. The function copies words 0, 3 and 4 into the
 * live HCGE context; word 0 must remain RGB565 (1). */
struct sf3000_setting_raw {
    uint32_t color_format, screen_w, screen_h;
    uint32_t img_w, img_h, img_pitch;
};

/* Optional diagnostic hook for the private HCGE state submission.  The stock
 * SF3000 driver calls hcge_set_state through its MIPS GOT.  Swapping that GOT
 * entry lets us observe the real rectangle after fb_paint_task builds it,
 * without changing the driver text or its render-thread ABI.  Geometry writes
 * Geometry writes remain explicitly gated by hcge_state_hook_apply.flag so
 * diagnostics cannot black-screen the device. */
static void sf3000_hcge_set_state_hook(void *ctx, void *state, unsigned int accel)
{
    volatile uint32_t *s = (volatile uint32_t *)state;
    unsigned int n = ++g_hcge_hook_calls;
    if (state && (n <= 12 || (n & 255u) == 0))
        DBG("DBG HCGE hook #%u ctx=%p state=%p accel=0x%x rect=%u,%u %ux%u mode=%u\n",
            n, ctx, state, accel, s[0xb0/4], s[0xb4/4], s[0xb8/4],
            s[0xbc/4], s[0xc8/4]);

    /* The rectangle is now known to be the active post-rotation destination.
     * Apply the requested hardware viewport before submitting state. */
    if (state && access("/mnt/sdcard/hcge_state_hook_apply.flag", F_OK) == 0) {
        int ow = 854, oh = 480;
        if (g_panel_scale == 0) {
            /* The state is the post-rotation landscape destination. */
            unsigned int sw = s[0x90/4], sh = s[0x94/4];
            int nx = sw ? 854 / (int)sw : 1, ny = sh ? 480 / (int)sh : 1;
            int mul = nx < ny ? nx : ny;
            if (mul < 1) mul = 1;
            ow = (int)sw * mul; oh = (int)sh * mul;
        } else if (g_panel_scale == 1 && g_aspect_num > 0 && g_aspect_den > 0) {
            ow = (480 * g_aspect_num + g_aspect_den / 2) / g_aspect_den;
            if (ow < 1) ow = 1;
            if (ow > 854) ow = 854;
        }
        s[0xb0/4] = (unsigned int)((854 - ow) / 2);
        s[0xb4/4] = (unsigned int)((480 - oh) / 2);
        s[0xb8/4] = (unsigned int)ow;
        s[0xbc/4] = (unsigned int)oh;
        DBG("DBG HCGE hook APPLY dst=%d,%d %dx%d\n",
            (854 - ow) / 2, (480 - oh) / 2, ow, oh);
    }
    if (g_hcge_set_state_orig)
        g_hcge_set_state_orig(ctx, state, accel);
}

static int hwdisp_install_hcge_state_hook(void)
{
    extern int sf3000_is_r36sx(void);
    if (sf3000_is_r36sx() || !g_handle ||
        access("/mnt/sdcard/hcge_state_hook.flag", F_OK) != 0)
        return 0;
    void *sym = dlsym(g_handle, "hcge_set_state");
    if (!sym) { DBG("DBG HCGE hook: symbol missing\n"); return -1; }
    struct link_map *lm = NULL;
    if (dlinfo(g_handle, RTLD_DI_LINKMAP, &lm) != 0 || !lm || !lm->l_ld) {
        DBG("DBG HCGE hook: link_map unavailable\n"); return -1;
    }
    ElfW(Addr) got_addr = 0; unsigned long local = 0, symno = 0, gotsym = 0;
    for (ElfW(Dyn) *d = lm->l_ld; d->d_tag != DT_NULL; ++d) {
        if (d->d_tag == DT_PLTGOT) got_addr = d->d_un.d_ptr;
        else if (d->d_tag == DT_MIPS_LOCAL_GOTNO) local = d->d_un.d_val;
        else if (d->d_tag == DT_MIPS_SYMTABNO) symno = d->d_un.d_val;
        else if (d->d_tag == DT_MIPS_GOTSYM) gotsym = d->d_un.d_val;
    }
    if (!got_addr || !local || symno < gotsym) {
        DBG("DBG HCGE hook: invalid GOT metadata got=%p local=%lu sym=%lu/%lu\n",
            (void *)got_addr, local, symno, gotsym); return -1;
    }
    size_t count = local + (symno - gotsym);
    volatile ElfW(Addr) *got = (volatile ElfW(Addr) *)got_addr;
    /* fb_paint_task's SF3000 binary loads hcge_set_state with
     *     lw t9,-32480(gp)
     * and its gp setup makes that the fourth word of the GOT (offset
     * 0x10).  The old symbol scan misses this slot on some loader builds:
     * the slot is pre-resolved before dlsym() returns and its value is not
     * necessarily the same function descriptor.  Prefer the known callsite
     * slot, but retain the metadata scan for driver revisions. */
    size_t candidate = (size_t)-1;
    if (lm->l_addr && got_addr == (ElfW(Addr))(lm->l_addr + 0x23360)) {
        candidate = 2 + 2; /* GOT[4], corresponding to gp-32480 in fb_paint_task */
        if (candidate >= count || (void *)(uintptr_t)got[candidate] != sym) {
            DBG("DBG HCGE hook: known GOT[4]=%p expected=%p; scanning\n",
                (void *)(uintptr_t)(candidate < count ? got[candidate] : 0), sym);
            candidate = (size_t)-1;
        }
    }
    for (size_t pass = 0; pass < 2; ++pass) {
        size_t begin = pass == 0 && candidate != (size_t)-1 ? candidate : 0;
        size_t end = pass == 0 && candidate != (size_t)-1 ? candidate + 1 : count;
        for (size_t i = begin; i < end; ++i) {
            if ((void *)(uintptr_t)got[i] != sym) continue;
        g_hcge_set_state_orig = (fn_hcge_set_state_t)(uintptr_t)got[i];
        long ps = sysconf(_SC_PAGESIZE); if (ps <= 0) ps = 4096;
        uintptr_t page = (uintptr_t)&got[i] & ~((uintptr_t)ps - 1);
        if (mprotect((void *)page, (size_t)ps, PROT_READ|PROT_WRITE) != 0) {
            DBG("DBG HCGE hook: mprotect failed errno=%d\n", errno); return -1;
        }
        got[i] = (ElfW(Addr))(uintptr_t)sf3000_hcge_set_state_hook;
        __builtin___clear_cache((char *)&got[i], (char *)&got[i] + sizeof(got[i]));
        mprotect((void *)page, (size_t)ps, PROT_READ);
        DBG("DBG HCGE hook: installed GOT[%zu] orig=%p hook=%p\n", i,
            (void *)g_hcge_set_state_orig, (void *)sf3000_hcge_set_state_hook);
        return 1;
        }
    }
    DBG("DBG HCGE hook: GOT target not found sym=%p entries=%zu\n", sym, count);
    return -1;
}

/* HCR-RTOS/HCFB exposes the scaler as a normal framebuffer ioctl.  The
 * SF3000 kernel may or may not carry this interface (and the ioctl number is
 * not present in the Linux fb headers), so probe it conservatively after the
 * stock driver has initialized fb0.  We first re-submit the driver's current
 * portrait transform; this is a no-op visually but tells us in log.txt
 * whether this kernel accepts the interface before we try changing ratios. */
struct sf3000_hcfb_scale { uint16_t h_div, v_div, h_mul, v_mul; };
#ifndef HCFBIOSET_SCALE
#define HCFBIOSET_SCALE _IOW(13, 0, struct sf3000_hcfb_scale)
#endif
#ifndef HCFBIOGET_SCALE_ONOFF
#define HCFBIOGET_SCALE_ONOFF _IOR(13, 6, unsigned long)
#endif

static void hwdisp_probe_hcfb_scale(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { DBG("DBG HCFB probe: open fb0 failed errno=%d\n", errno); return; }
    unsigned long enabled = 0;
    int gr = ioctl(fd, HCFBIOGET_SCALE_ONOFF, &enabled);
    DBG("DBG HCFB probe: GET_SCALE_ONOFF ret=%d errno=%d value=%lu\n", gr, errno, enabled);
    struct sf3000_hcfb_scale s = { 640, 480, 480, 854 };
    int sr = ioctl(fd, HCFBIOSET_SCALE, &s);
    DBG("DBG HCFB probe: SET_SCALE(640,480,480,854) ret=%d errno=%d\n", sr, errno);
    close(fd);
}

static void hwdisp_raw_viewport(int src_w, int src_h, int pitch) {
    extern int sf3000_is_r36sx(void);
    if (sf3000_is_r36sx() || src_w <= 0 || src_h <= 0) return;
    int out_w = 854, out_h = 480;
    if (g_panel_scale == 0) { /* Integer: largest exact source multiple. */
        int n = 854 / src_w, ny = 480 / src_h;
        if (ny < n) n = ny;
        if (n < 1) n = 1;
        out_w = src_w * n; out_h = src_h * n;
    } else if (g_panel_scale == 1 && g_aspect_num > 0 && g_aspect_den > 0) {
        out_h = 480;
        out_w = (out_h * g_aspect_num + g_aspect_den / 2) / g_aspect_den;
        if (out_w > 854) out_w = 854;
        if (out_w < 1) out_w = 1;
    }
    static int last_w, last_h, last_src_w, last_src_h, last_pitch;
    if (out_w == last_w && out_h == last_h && src_w == last_src_w &&
        src_h == last_src_h && pitch == last_pitch) return;
    if (!p_setting) return;
    struct sf3000_setting_raw setting = {
        1u, (uint32_t)out_w, (uint32_t)out_h,
        (uint32_t)src_w, (uint32_t)src_h, (uint32_t)pitch
    };
    p_setting(&setting);
    last_w = out_w; last_h = out_h;
    last_src_w = src_w; last_src_h = src_h; last_pitch = pitch;
    DBG("DBG HCGE viewport setting: src=%dx%d pitch=%d out=%dx%d color_format=%u\n",
        src_w, src_h, pitch, out_w, out_h, setting.color_format);
}

static int hwdisp_driver_present(void *src, int w, int h, int pitch) {
    extern int sf3000_is_r36sx(void);
    volatile unsigned int *dbg_ctx = (p_g_render && *p_g_render) ?
        (volatile unsigned int *)*p_g_render : NULL;
    /* With the guarded canvas patch enabled, preserve the driver's menu
     * envelope (320x240 is composed through its fixed 640x480 canvas), but
     * program game-sized frames before signalling the render thread. */
    if (access("/mnt/sdcard/hcge_canvas_patch.flag", F_OK) == 0 &&
        p_g_render && *p_g_render && !sf3000_is_r36sx()) {
        int ow = 854, oh = 480;
        if ((w == 320 && h == 240) || (w == 854 && h == 480)) { ow = 640; oh = 480; }
        if (g_panel_scale == 0) {
            int n = 854 / w, ny = 480 / h;
            if (ny < n) n = ny;
            if (n < 1) n = 1;
            ow = w * n; oh = h * n;
        } else if (g_panel_scale == 1 && g_aspect_num > 0 && g_aspect_den > 0) {
            ow = (480*g_aspect_num + g_aspect_den/2)/g_aspect_den;
            if (ow < 1) ow = 1;
            if (ow > 854) ow = 854;
        }
        volatile unsigned int *ctx = (volatile unsigned int *)*p_g_render;
        ctx[0x3294/4] = (unsigned int)ow; ctx[0x3290/4] = (unsigned int)oh;
        DBG("DBG experimental viewport-pre: src=%dx%d dst=%dx%d\n", w, h, ow, oh);
    }
    /* Let disp_frame perform any required HCGE initialization first. */
    hwdisp_raw_viewport(w, h, pitch);
    int rv = p_disp(src, w, h, pitch);
    /* disp_frame may restore the stock scaler tuple during initialization;
     * apply the requested HCFB tuple after it, for the following frame. */
    /* HCFB writes are intentionally disabled: the ioctl accepts the tuple but
     * the current orientation hypothesis squishes both menu and game output.
     * Keep the probe/logging active until the portrait tuple is verified. */
    /* The private driver rewrites the canvas during disp_frame().  Update the
     * live canvas after that write so the following frame uses the requested
     * hardware viewport. This is guarded by the existing diagnostic marker. */
    if (dbg_ctx && !sf3000_is_r36sx() &&
        access("/mnt/sdcard/hcge_canvas_patch.flag", F_OK) == 0) {
        unsigned ow = 854, oh = 480;
        if (g_panel_scale == 0) {
            int n = 854 / w, ny = 480 / h;
            if (ny < n) n = ny;
            if (n < 1) n = 1;
            ow = (unsigned)(w * n); oh = (unsigned)(h * n);
        } else if (g_panel_scale == 1 && g_aspect_num > 0 && g_aspect_den > 0) {
            ow = (unsigned)((480 * g_aspect_num + g_aspect_den / 2) / g_aspect_den);
            if (ow < 1) ow = 1;
            if (ow > 854) ow = 854;
        }
        dbg_ctx[0x3294/4] = ow;
        dbg_ctx[0x3290/4] = oh;
    }
    return rv;
}

int hwdisp_native_game_surface(void) { return g_native_game_surface; }

static void hwdisp_set_driver_fit(int fit, int force_log) {
    if (!p_aspect) return;
    /* The API argument is the inverse of the driver's stored fullscreen bit. */
    int arg = fit ? 1 : 0;
    if (arg == g_fs_state && !force_log) return;
    p_aspect(arg);
    g_fs_state = arg;
    int live_old = -1, live_new = -1;
    if (p_g_render && *p_g_render) {
        /* This is the driver's real context layout (verified against
         * driver_sf3000.so fbdev_init/fb_paint_task). The render thread only
         * consumes this copied field, not the global changed above. */
        /* fbdev_video_aspect_ratio() stores the live render flag at
         * context+0x32b0 (12976), as verified from driver_sf3000.so:
         *   sw v1,12976(v0), where v0 is g_render. */
        volatile unsigned int *live_fullscreen =
            (volatile unsigned int *)((unsigned char *)*p_g_render + 0x32b0);
        live_old = (int)*live_fullscreen;
        *live_fullscreen = (unsigned int)!arg;
        live_new = (int)*live_fullscreen;
    }
    DBG("DBG driver aspect: api_arg=%d global_fullscreen=%d live_ctx=%p live_fullscreen=%d->%d mode=%s\n",
        arg, !arg, p_g_render ? (void *)(p_g_render ? *p_g_render : NULL) : NULL,
        live_old, live_new, fit ? "aspect-fit" : "stretch-fill");
}

/* Edge-sharpen level 0-10 (p4 of fbdev_set_enhance; colours left neutral at 50).
 * Whole-panel DIS post-process — the closest the HW gets to "nearest" (edge
 * peaking, not true point sampling). Applied on change only (it's an ioctl). */
void hwdisp_set_sharpen(int level) {
    if (level < 0) level = 0;
    if (level > 10) level = 10;
    if (!p_enhance || level == g_sharpen) return;
    p_enhance(50, 50, 50, 50, level);
    g_sharpen = level;
}

/* Aspect-pad staging buffer (lazy alloc, resized on demand) */
static uint16_t *g_pad_buf  = NULL;
static int       g_pad_cap  = 0;
static int       g_pad_w    = 0;
static int       g_pad_h    = 0;

/* Nearest-upscale buffer (always 1280x720) */
static uint16_t *g_near_buf = NULL;

static int g_aspect_num = 0;
static int g_aspect_den = 0;
static int g_filter_nearest = 0;   /* SW-scale path (true nearest OR sharp) */
static int g_filter_sharp   = 0;   /* sharp variant: integer prescale + HW residual */

int hwdisp_init(void) {
    extern void sf3000_dump_fb_state(const char *);
    if (g_active) return 0;
    sf3000_dump_fb_state("hwdisp_init/pre");

    /* Device-specific driver: each device ships its own driver.so build (panel
     * init + render behavior differ). Single source of truth in plat_sdl.c;
     * fall back to a generic driver.so if the per-device file is absent. */
    extern const char *sf3000_driver_path(void);
    const char *drv = sf3000_driver_path();
    g_handle = dlopen(drv, RTLD_NOW | RTLD_GLOBAL);
    if (!g_handle) {
        DBG("DBG hwdisp: dlopen %s failed (%s), trying driver.so\n", drv, dlerror());
        g_handle = dlopen("/mnt/sdcard/cubegm/driver.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_handle) {
        fprintf(stderr, "hwdisp: dlopen failed: %s\n", dlerror());
        return -1;
    }
    DBG("DBG hwdisp: loaded %s\n", drv);

    p_init   = (fn_init_t)  dlsym(g_handle, "video_drivers_init");
    p_deinit = (fn_deinit_t)dlsym(g_handle, "video_driver_deinit");
    p_disp   = (fn_disp_t)  dlsym(g_handle, "video_driver_disp_frame");
    p_g_render = (void **)dlsym(g_handle, "g_render");
    p_aspect = (fn_aspect_t)dlsym(g_handle, "fbdev_video_aspect_ratio");
    p_setting = (fn_setting_t)dlsym(g_handle, "video_driver_setting");
    p_setmode = (fn_setmode_t)dlsym(g_handle, "video_driver_setmode");
    p_enhance = (fn_enhance_t)dlsym(g_handle, "fbdev_set_enhance");
    g_native_game_surface = 0;
    g_fs_state = -1;
    g_fs_num = g_fs_den = -1;

    if (!p_init || !p_deinit || !p_disp) {
        fprintf(stderr, "hwdisp: dlsym failed (init=%p deinit=%p disp=%p g_render=%p)\n",
                p_init, p_deinit, p_disp, p_g_render);
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }

    /* Keep the stock 640x480 game canvas. The driver’s aspect-fit path uses
     * that virtual surface; forcing it to 854x480 makes every mode fullscreen
     * before the hardware scaler can preserve the requested ratio. */
    /* Install before video_drivers_init: it creates the paint thread and the
     * first HCGE submission can happen during initialization. */
    hwdisp_install_hcge_state_hook();
    int rv = p_init();
    if (rv <= 0) {
        fprintf(stderr, "hwdisp: video_drivers_init returned %d\n", rv);
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }
    g_active = 1;
    fprintf(stderr, "hwdisp: HW path active (init rv=%d g_render=%p ctx=%p)\n",
            rv, (void *)p_g_render, p_g_render ? *p_g_render : NULL);

    /* R36SX: enable the HCGE engine via video_driver_setmode so disp_frame's
     * engine-sync doesn't hang (rkgame configures before presenting). disp_frame
     * then HW-scales src→panel (no CPU upscale). Try a few mode args; logged
     * fsync'd so a hang still tells us how far it got. */
    extern int sf3000_is_r36sx(void);
    if (sf3000_is_r36sx()) {
        typedef int (*fn_setmode_t)(int, int);
        fn_setmode_t p_setmode = (fn_setmode_t)dlsym(g_handle, "video_driver_setmode");
        DBG("DBG hwdisp: setmode=%p calling setmode(0,0)\n", (void*)p_setmode);
        if (p_setmode) { int sr = p_setmode(0, 0); DBG("DBG hwdisp: setmode(0,0) ret=%d\n", sr); }
    }
    if (!sf3000_is_r36sx()) hwdisp_probe_hcfb_scale();
    g_sharpen = -1;   /* force re-apply on the next hwdisp_set_sharpen() */
    sf3000_dump_fb_state("hwdisp_init/post");
    /* fb0 for direct presents is mmap'd lazily by hwdisp_present_direct(). */
    return 0;
}

int hwdisp_active(void) { return g_active; }

/* The stock shutdown/logo process renders through fb0 after the HCGE driver
 * exits.  Leave that framebuffer clean; otherwise the previous panel-sized
 * FrogUI frame can remain visible around the logo for a few refreshes. */
static void clear_fb0_for_shutdown(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) return;
    struct fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0 || fi.smem_len < sizeof(uint32_t)) {
        close(fd);
        return;
    }
    void *mem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem != MAP_FAILED) {
        uint32_t *p = (uint32_t *)mem;
        size_t n = fi.smem_len / sizeof(*p);
        for (size_t i = 0; i < n; i++) p[i] = 0xFF000000u;
        msync(mem, fi.smem_len, MS_SYNC);
        munmap(mem, fi.smem_len);
    }
    /* Ensure the cleared fb0 is the active layer before the shutdown process
     * starts drawing its logo. */
    int dis = open("/dev/dis", O_RDWR);
    if (dis >= 0) {
        struct { int a, b, c; } route = {1, 0, 0};
        ioctl(dis, 0xc00c0e0c, &route);
        close(dis);
    }
    close(fd);
}

void hwdisp_set_target_aspect(int num, int den) {
    int changed = num != g_aspect_num || den != g_aspect_den;
    g_aspect_num = num;
    g_aspect_den = den;
    /* SF3000's HCFB scaler is portrait before the panel's 270-degree
     * transform: 640x480 input maps to 480x854 output.  Unlike the private
     * video_driver_setting() helper, this ioctl actually controls the live
     * framebuffer layer.  Program only when the requested landscape width
     * changes; keep the panel height at 480 and let the driver center bars. */
    extern int sf3000_is_r36sx(void);
    if (!sf3000_is_r36sx() && p_setmode && den > 0) {
        int mode = -1;
        if (num * 3 == den * 4) mode = 1;       /* stock driver: 4:3 */
        else if (num * 9 == den * 16) mode = 0; /* stock driver: 16:9 */
        static int last_mode = -2;
        if (mode >= 0 && mode != last_mode) {
            int rc = p_setmode(0, mode);
            fprintf(stderr, "TFDBG driver setmode: mode=%d rc=%d\n", mode, rc);
            last_mode = mode;
        }
    }
    if (changed && !sf3000_is_r36sx() && den > 0) {
        int out_w = (480 * num + den / 2) / den;
        if (out_w < 1) out_w = 1;
        if (out_w > 854) out_w = 854;
        DBG("DBG target aspect: %d/%d HCFB tuple 640,480,480,%d\n",
            num, den, out_w);
        fprintf(stderr, "TFDBG target aspect: %d/%d HCFB tuple 640,480,480,%d\n",
                num, den, out_w);
    } else if (changed) {
        DBG("DBG target aspect: %d/%d (HCFB probe skipped)\n", num, den);
    }
}

/* filter: scale_filter enum — 0 nearest, 1 bilinear, 2 sharp (integer prescale
 * + HW residual). Nearest and Sharp both use the SW path; Sharp additionally
 * sets g_filter_sharp so present_direct prescales instead of full-stretching. */
void hwdisp_set_filter(int filter) {
    g_filter_nearest = (filter != 1);   /* nearest(0) or sharp(2) */
    g_filter_sharp   = (filter == 2);
    /* If switching to a SW-scale filter, ensure native buffer exists. */
    if (g_filter_nearest && !g_near_buf) {
        g_near_buf = (uint16_t*)malloc(HW_BUFSZ);
        if (g_near_buf) memset(g_near_buf, 0, HW_BUFSZ);
    }
}

/* Pad horizontally: src(w×h) → g_pad_buf(pad_w×h), src centered, sides black. */
static void pad_horizontal(const void *src, int w, int h, int pitch_bytes, int pad_w) {
    int need = pad_w * h;
    if (need > g_pad_cap) {
        free(g_pad_buf);
        g_pad_cap = need + 4096;
        g_pad_buf = (uint16_t*)malloc(g_pad_cap * sizeof(uint16_t));
        g_pad_h   = 0;
        g_pad_w   = 0;
    }
    if (!g_pad_buf) return;

    int off_x = (pad_w - w) / 2;
    if (off_x < 0) off_x = 0;

    if (pad_w != g_pad_w || h != g_pad_h) {
        memset(g_pad_buf, 0, (size_t)pad_w * h * sizeof(uint16_t));
        g_pad_w = pad_w;
        g_pad_h = h;
    }

    for (int y = 0; y < h; y++) {
        const uint16_t *srow = (const uint16_t *)((const char *)src + y * pitch_bytes);
        uint16_t *drow = g_pad_buf + (size_t)y * pad_w + off_x;
        memcpy(drow, srow, (size_t)w * sizeof(uint16_t));
    }
}


/* Direct present: write the frame straight into fb0; the display controller
 * scales fb0 → panel. Bypasses video_driver_disp_frame, which HANGS on R36SX
 * and ABORTS on SF3000 panel-size input. Used for FrogUI (both devices) and all
 * R36SX frames. Returns 1 if presented. */
/* Panel scale mode for R36SX disp_frame present: 0=integer(NONE), 1=aspect, 2=full.
 * disp_frame itself aspect-fits src→panel, so:
 *   aspect → pass src straight (driver aspect-fits, bars).
 *   full   → SW-stretch src into a 640x480 panel buffer → disp_frame 1:1 (fills).
 *   integer→ SW integer-replicate src centered in 640x480 buffer → disp_frame 1:1. */
#define PANEL_PW 640
#define PANEL_PH 480
static int g_panel_scale = 2;       /* default full */
void hwdisp_set_panel_scale(int m) { g_panel_scale = m; }

/* Cheap Integer-mode staging shared by SF-class and R36SX. Instead of expanding
 * to the complete panel in software, center the native frame in a panel/N
 * envelope and let HCGE supply the N enlargement. Keep two buffers because the
 * driver's DMA can still be reading the previously submitted frame. */
#define INT_ENV_MAX_W 854
#define INT_ENV_MAX_H 480
static uint16_t *g_int_env[2];
static int g_int_env_i;
static int g_int_env_w[2], g_int_env_h[2];
static int g_int_src_w[2], g_int_src_h[2];

static uint16_t *integer_envelope_build(const void *src, int w, int h,
                                        int pitch_bytes, int panel_w, int panel_h,
                                        int *out_w, int *out_h) {
    int nx = panel_w / w, ny = panel_h / h;
    int n = nx < ny ? nx : ny;
    if (n < 1) n = 1;

    int ew = (panel_w + n - 1) / n;
    int eh = (panel_h + n - 1) / n;
    /* Odd source widths wedge some driver builds. The at-most-one-pixel wider
     * envelope changes horizontal scale by under 0.5% for common modes. */
    ew = (ew + 1) & ~1;
    if (ew < w) ew = (w + 1) & ~1;
    if (eh < h) eh = h;
    if (ew > INT_ENV_MAX_W || eh > INT_ENV_MAX_H) return NULL;

    if (!g_int_env[0])
        g_int_env[0] = (uint16_t *)malloc(INT_ENV_MAX_W * INT_ENV_MAX_H * 2);
    if (!g_int_env[1])
        g_int_env[1] = (uint16_t *)malloc(INT_ENV_MAX_W * INT_ENV_MAX_H * 2);
    if (!g_int_env[0] || !g_int_env[1]) return NULL;

    int bi = g_int_env_i;
    g_int_env_i ^= 1;
    uint16_t *dst = g_int_env[bi];
    if (g_int_env_w[bi] != ew || g_int_env_h[bi] != eh ||
        g_int_src_w[bi] != w || g_int_src_h[bi] != h) {
        memset(dst, 0, (size_t)ew * eh * 2);
        g_int_env_w[bi] = ew; g_int_env_h[bi] = eh;
        g_int_src_w[bi] = w;  g_int_src_h[bi] = h;
    }

    int ox = (ew - w) / 2, oy = (eh - h) / 2;
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)(oy + y) * ew + ox,
               (const uint8_t *)src + (size_t)y * pitch_bytes,
               (size_t)w * 2);
    *out_w = ew; *out_h = eh;
    return dst;
}

/* Ping-pong output buffers: disp_frame DMA-reads the previous frame async, so we
 * write the other buffer — the result is safe to hand straight to disp_frame,
 * no extra staging copy (present_direct skips its copy for panel_build output).
 * Only the source rows that differ are scaled; duplicated rows are memcpy'd, and
 * the full-buffer memset happens only when the output geometry changes. */
/* mode: 0=integer replicate (exact NxN, centered), 1=aspect-fit nearest stretch
 * (centered, bars), 2=full nearest stretch (fills panel). All nearest → sharp. */
static uint16_t *panel_build(const void *src, int w, int h, int pitch_bytes, int mode) {
    static uint16_t *pb[2]; static unsigned pbgeo[2]; static int pbi;
    pbi ^= 1;
    if (!pb[pbi]) { pb[pbi] = (uint16_t*)malloc(PANEL_PW*PANEL_PH*2); if (!pb[pbi]) return NULL; pbgeo[pbi] = 0; }
    uint16_t *d = pb[pbi];
    const int sp = pitch_bytes/2; const uint16_t *s = (const uint16_t*)src;
    if (mode == 0) {
        int n = PANEL_PW/w; int ny = PANEL_PH/h; if (ny<n) n=ny; if (n<1) n=1;
        int dw=w*n, dh=h*n, ox=(PANEL_PW-dw)/2, oy=(PANEL_PH-dh)/2;
        unsigned geo = ((unsigned)dw<<16)|(unsigned)dh;
        if (pbgeo[pbi] != geo) { memset(d, 0, PANEL_PW*PANEL_PH*2); pbgeo[pbi] = geo; }
        for (int y=0; y<h; y++){ const uint16_t *sr=s+(size_t)y*sp;
            uint16_t *dr=d+(size_t)(oy+y*n)*PANEL_PW+ox;
            if (n == 2) {
                /* Fast 2x: one 32-bit store per src pixel (two dup pixels at
                 * once) instead of two 16-bit stores. dr is 32-bit aligned
                 * (PANEL_PW, y*n, and ox=(PANEL_PW-w*2)/2 are all even). */
                uint32_t *d32 = (uint32_t *)dr;
                for (int x=0;x<w;x++){ uint32_t px=sr[x]; d32[x]=(px<<16)|px; }
            } else {
                for (int x=0;x<w;x++){ uint16_t px=sr[x]; uint16_t *dp=dr+x*n; for(int rx=0;rx<n;rx++) dp[rx]=px; }
            }
            for (int ry=1; ry<n; ry++) memcpy(dr+(size_t)ry*PANEL_PW, dr, (size_t)dw*2); }
    } else { /* nearest stretch: full fills the panel, aspect fits centered w/ bars */
        int dw = PANEL_PW, dh = PANEL_PH, ox = 0, oy = 0;
        if (mode == 1) {                       /* aspect-fit */
            if (w * PANEL_PH >= h * PANEL_PW) dh = h * PANEL_PW / w;
            else                              dw = w * PANEL_PH / h;
            ox = (PANEL_PW - dw) / 2; oy = (PANEL_PH - dh) / 2;
        }
        unsigned geo = ((unsigned)dw<<16)|(unsigned)dh;
        if (pbgeo[pbi] != geo) { memset(d, 0, PANEL_PW*PANEL_PH*2); pbgeo[pbi] = geo; }
        /* Src-driven run lengths: dst column x maps to src x*w/dw (nearest,
         * monotonic), so each src pixel covers a run of rl[sx] dst pixels. One
         * sequential read per src pixel (no gather), and runs are written with
         * 32-bit stores when aligned. */
        static int rl[PANEL_PW]; static int lw=-1, ldw=-1;
        if (w!=lw || dw!=ldw){ int prev=0; for(int sx=0;sx<w;sx++){ int e=(sx+1)*dw/w; rl[sx]=e-prev; prev=e; } lw=w; ldw=dw; }
        int lsy = -1;
        for (int y=0;y<dh;y++){
            int sy = y*h/dh;
            uint16_t *dr=d+(size_t)(oy+y)*PANEL_PW+ox;
            if (sy == lsy) { memcpy(dr, dr-PANEL_PW, (size_t)dw*2); continue; }
            lsy = sy;
            const uint16_t *sr=s+(size_t)sy*sp;
            uint16_t *dp=dr;
            for (int sx=0; sx<w; sx++){
                uint16_t px=sr[sx]; int r=rl[sx];
                uint32_t px2=((uint32_t)px<<16)|px;
                while (r>=2 && !((uintptr_t)dp&3)){ *(uint32_t*)dp=px2; dp+=2; r-=2; }
                while (r-->0) *dp++=px;
            }
        }
    }
    return d;
}

/* Sharp-bilinear prescale: integer-replicate src by n (fast word-store path)
 * into a tight n*w × n*h buffer. The GE then does only the small leftover
 * fractional stretch to the panel (fill/fit) in HW — far cheaper than a full SW
 * stretch to 640x480, and near-nearest sharp (pixels already n×-doubled). */
static uint16_t *prescale_int(const void *src, int w, int h, int pitch_bytes,
                              int n, int *out_w, int *out_h) {
    static uint16_t *pb[2]; static int pbi;
    pbi ^= 1;
    if (!pb[pbi]) { pb[pbi] = (uint16_t*)malloc(PANEL_PW*PANEL_PH*2); if (!pb[pbi]) return NULL; }
    uint16_t *d = pb[pbi];
    const int sp = pitch_bytes/2; const uint16_t *s = (const uint16_t*)src;
    int dw = w * n;
    for (int y=0; y<h; y++){
        const uint16_t *sr = s + (size_t)y*sp;
        uint16_t *dr = d + (size_t)(y*n)*dw;
        if (n == 2) {
            uint32_t *d32 = (uint32_t *)dr;
            int x = 0;
            /* Read 2 src pixels per iter (one 32-bit load vs two 16-bit) when
             * the src row is 32-bit aligned; emit two doubled dst words. */
            if (!(((uintptr_t)sr) & 3)) {
                const uint32_t *s32 = (const uint32_t *)sr;
                for (; x + 1 < w; x += 2) {
                    uint32_t two = s32[x>>1];
                    uint32_t a = two & 0xffff, b = two >> 16;
                    d32[x]   = (a<<16)|a;
                    d32[x+1] = (b<<16)|b;
                }
            }
            for (; x < w; x++){ uint32_t px=sr[x]; d32[x]=(px<<16)|px; }
        } else {
            for (int x=0;x<w;x++){ uint16_t px=sr[x]; uint16_t *dp=dr+x*n; for(int k=0;k<n;k++) dp[k]=px; }
        }
        for (int ry=1; ry<n; ry++) memcpy(dr+(size_t)ry*dw, dr, (size_t)dw*2);
    }
    *out_w = dw; *out_h = h*n;
    return d;
}

int hwdisp_present_direct(const void *src, int w, int h, int pitch_bytes) {
    static int s_n = 0;
    int lg = (s_n < 8);
    s_n++;
    if (!g_active || !src || !p_disp) return 0;
    /* disp_frame HW-scales src→panel; the driver's fullscreen flag
     * (fbdev_video_aspect_ratio, like stock) picks fill vs aspect-fit. So:
     *   full   → flag=fill, pass src straight → GE distort-fills in HW (fast).
     *   aspect → flag=fit,  pass src straight → GE letterboxes in HW.
     *   integer→ SW integer-replicate centered in 640x480 (exact NxN, sharp) —
     *            the GE can't do nearest. panel-size (FrogUI) passes straight.
     * Only integer still pays SW cost. */
    /* Filter route (game frames only; FrogUI/menu panel-size frames pass straight):
     *   Bilinear → hand src straight to the GE, fullscreen flag picks fill/fit.
     *              Fast (HW scale), but soft.
     *   Nearest  → SW-scale into a 640x480 panel buffer (integer replicate /
     *              aspect-fit / full-fill), present 1:1. True sharp, costs CPU. */
    int game = !(w == PANEL_PW && h == PANEL_PH);
    int sw_nearest = game && g_filter_nearest;
    int sw_integer = game && !g_filter_nearest && g_panel_scale == 0;

    const void *psrc = src; int pw = w, ph = h, ppitch = pitch_bytes;
    int staged = 0;
    int integer_hw = 0;
    int hw_scale = !sw_nearest;   /* whether the GE still scales (needs fill/fit flag) */

    if (sw_nearest && g_filter_sharp && g_panel_scale != 0 /*full/aspect*/) {
        /* Sharp: prescale by the largest N (>=2) that fits the panel, then let
         * the GE do the small residual stretch (fill for full, fit for aspect).
         * Halves the SW cost vs a full 640x480 stretch and reuses the fast
         * integer path. N=1 (source already big) → fall back to true stretch. */
        int n = PANEL_PW / w; int ny = PANEL_PH / h; if (ny < n) n = ny;
        if (n >= 2) {
            int ow, oh;
            uint16_t *b = prescale_int(src, w, h, pitch_bytes, n, &ow, &oh);
            if (b) { psrc = b; pw = ow; ph = oh; ppitch = ow*2; staged = 1; hw_scale = 1; }
        }
        if (!staged) {   /* N<2 or alloc fail: true SW stretch to panel */
            uint16_t *b = panel_build(src, w, h, pitch_bytes, g_panel_scale);
            if (b) { psrc = b; pw = PANEL_PW; ph = PANEL_PH; ppitch = PANEL_PW*2; staged = 1; }
        }
    } else if (sw_integer) {
        /* Bilinear + Integer: submit the small panel/N envelope and let HCGE
         * enlarge it. This replaces v1.0.12's expensive full 640x480 SW blit. */
        int ow, oh;
        uint16_t *b = integer_envelope_build(src, w, h, pitch_bytes,
                                             PANEL_PW, PANEL_PH, &ow, &oh);
        if (b) {
            psrc = b; pw = ow; ph = oh; ppitch = ow * 2;
            staged = 1; hw_scale = 1; integer_hw = 1;
        }
    } else if (sw_nearest) {
        /* True nearest (full/aspect SW stretch) or exact NxN integer — present
         * the 640x480 panel buffer 1:1, no HW scaling. */
        uint16_t *b = panel_build(src, w, h, pitch_bytes, g_panel_scale);
        if (b) { psrc = b; pw = PANEL_PW; ph = PANEL_PH; ppitch = PANEL_PW*2; staged = 1; }
    }

    if (p_aspect && hw_scale) {
        int fit = integer_hw ? 0 : (g_panel_scale != 2);
        hwdisp_set_driver_fit(fit, 0);
    }
    /* disp_frame DMA-reads src asynchronously; handing it the caller's live
     * buffer races the next frame's rendering (font/pixel shimmer during menu
     * scrolling on R36SX). Stage into ping-pong buffers so the engine always
     * scans a stable copy. panel_build output is already ping-ponged — skip. */
    static uint16_t *g_dpp[2];
    static int g_dppi;
    if (!g_dpp[0]) { g_dpp[0] = (uint16_t*)malloc(PANEL_PW*PANEL_PH*2); g_dpp[1] = (uint16_t*)malloc(PANEL_PW*PANEL_PH*2); }
    if (!staged && g_dpp[0] && g_dpp[1] && pw <= PANEL_PW && ph <= PANEL_PH) {
        uint16_t *dst = g_dpp[g_dppi]; g_dppi ^= 1;
        for (int y = 0; y < ph; y++)
            memcpy(dst + (size_t)y*pw, (const uint8_t*)psrc + (size_t)y*ppitch, (size_t)pw*2);
        psrc = dst; ppitch = pw*2;
    }
    if (lg) DBG("DBG present_direct#%d: pre disp_frame %dx%d scale=%d\n", s_n, pw, ph, g_panel_scale);
    int rv = p_disp((void *)psrc, pw, ph, ppitch);
    if (lg) DBG("DBG present_direct#%d: post rv=%d\n", s_n, rv);
    return 1;
}

void hwdisp_present(const void *src, int w, int h, int pitch_bytes) {
    static int s_n = 0;
    int lg = (s_n < 8);
    s_n++;
    if (lg) DBG("DBG present#%d: src=%p w=%d h=%d pitch=%d active=%d p_disp=%p filt=%d asp=%d/%d HW=%dx%d\n",
                s_n, src, w, h, pitch_bytes, g_active, (void*)p_disp,
                g_filter_nearest, g_aspect_num, g_aspect_den, HW_W, HW_H);
    if (lg) fprintf(stderr, "TFDBG present#%d src=%p %dx%d pitch=%d filt=%d asp=%d/%d hw=%d\n",
                    s_n, src, w, h, pitch_bytes, g_filter_nearest,
                    g_aspect_num, g_aspect_den, g_active);
    if (!g_active || !src) { if (lg) DBG("DBG present#%d: EARLY-RET\n", s_n); return; }
    if (!p_disp) { if (lg) DBG("DBG present#%d: no p_disp\n", s_n); return; }
    /* SF-class presents use this path (not present_direct).  Keep the
     * driver's fill/aspect state synchronized with every menu transition;
     * previously only the R36SX direct path called p_aspect(), so SF3000
     * could remain stuck in Native/Fill after switching to Integer or back. */
    if (p_aspect) {
        int fit = (g_aspect_num > 0 && g_aspect_den > 0);
        int arg = fit ? 1 : 0;
        if (arg != g_fs_state || g_aspect_num != g_fs_num || g_aspect_den != g_fs_den) {
            hwdisp_set_driver_fit(fit, 1);
            g_fs_num = g_aspect_num;
            g_fs_den = g_aspect_den;
            DBG("DBG scaler mode: target=%d/%d api_arg=%d stored_fullscreen=%d\n",
                g_aspect_num, g_aspect_den, arg, !arg);
        }
    }
    int rv;
    /* SF-class hardware has no viable software-scaling budget.  Always submit
     * the native frame to the HCGE driver; its scaler remains the sole
     * presentation path.  The old nearest branch expanded every frame on the
     * MIPS CPU, causing severe slowdown/audio underruns and masking driver
     * geometry bugs.  Keep g_filter_nearest for menu compatibility/logging,
     * but never consume it here. */
    if (g_filter_nearest && lg)
        DBG("DBG present#%d: nearest requested; ignored on SF HCGE (HW-only)\n", s_n);

    /* Unpadded presents must NOT hand the core's live framebuffer to
     * disp_frame — its HCGE DMA reads the source asynchronously while the core
     * renders the next frame into it (bus contention + engine re-sync = the
     * "Full-screen lags" report; Aspect was accidentally immune because its
     * pad step copies to staging). Ping-pong stage, like present_direct. */
    static uint16_t *fs[2];
    static int fsi;
    const void *psrc = src;
    int ppitch = pitch_bytes;
    /* Cover the driver's full source envelope, not only 640x480 game frames.
     * FrogUI is 854x480 on SF3500; leaving that live buffer unstaged lets HCGE
     * race the next menu redraw and eventually deliver SIGBUS. */
    if (!fs[0]) { fs[0] = (uint16_t*)malloc(HW_BUFSZ); fs[1] = (uint16_t*)malloc(HW_BUFSZ); }
    if (fs[0] && fs[1] && w <= HW_W && h <= HW_H) {
        uint16_t *dst = fs[fsi]; fsi ^= 1;
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y*w, (const uint8_t*)src + (size_t)y*pitch_bytes, (size_t)w*2);
        psrc = dst; ppitch = w*2;
    }

    /* HW (bilinear) path: pass through, optional aspect pad. */
    if (g_aspect_num <= 0 || g_aspect_den <= 0) {
        if (lg) DBG("DBG present#%d: passthru pre p_disp(%p,%d,%d,%d)\n", s_n, psrc, w, h, ppitch);
        rv = hwdisp_driver_present((void *)psrc, w, h, ppitch);
        if (lg) DBG("DBG present#%d: passthru post p_disp rv=%d\n", s_n, rv);
        return;
    }

    /* SF3000/SF3500 must never reshape or pad the core frame in software.
     * The raw HCFB viewport below is the hardware crop/scale mechanism.  The
     * old pad_horizontal path produced the 292/398/426-wide NES frames seen
     * in the driver log and defeated the whole HCGE experiment. */
    {
        extern int sf3000_is_r36sx(void);
        if (!sf3000_is_r36sx()) {
            if (lg) DBG("DBG present#%d: SF-class raw viewport passthru\n", s_n);
            rv = hwdisp_driver_present((void *)psrc, w, h, ppitch);
            if (lg) DBG("DBG present#%d: raw viewport post rv=%d\n", s_n, rv);
            return;
        }
    }

    int pad_w = h * g_aspect_num / g_aspect_den;
    pad_w &= ~1;   /* odd width wedges disp_frame/HCGE to black on R36SX (e.g.
                    * 853 for 480-tall, 455 for 256-tall). Round to even. */
    if (pad_w <= w) {
        if (lg) DBG("DBG present#%d: nopad pre p_disp\n", s_n);
        rv = hwdisp_driver_present((void *)psrc, w, h, ppitch);
        if (lg) DBG("DBG present#%d: nopad post p_disp rv=%d\n", s_n, rv);
        return;
    }

    pad_horizontal(psrc, w, h, ppitch, pad_w);
    if (!g_pad_buf) {
        rv = hwdisp_driver_present((void *)psrc, w, h, ppitch);
        if (lg) DBG("DBG present#%d: padfail post rv=%d\n", s_n, rv);
        return;
    }
    if (lg) DBG("DBG present#%d: pad pre p_disp(pad_w=%d)\n", s_n, pad_w);
    rv = hwdisp_driver_present(g_pad_buf, pad_w, h, pad_w * 2);
    if (lg) DBG("DBG present#%d: pad post p_disp rv=%d\n", s_n, rv);
}

/* Panel-integer present for SF-class panels.
 *
 * v1.0.12 expanded both axes in software into a complete 854x480 frame.  That
 * made Integer the only aspect mode doing a full-panel CPU blit every frame and
 * was enough to starve emulation/audio on this small MIPS core.
 *
 * Submit the native frame inside a panel/N envelope and let HCGE supply the N
 * enlargement, so the visible image remains N high and approximately N wide
 * (854 is not divisible by 2 or 3). This cuts staging and DMA traffic by roughly
 * N squared while retaining the integer-sized viewport. */
void hwdisp_present_integer(const void *src, int w, int h, int pitch_bytes) {
    if (!g_active || !p_disp || !src) return;
    if (w <= 0 || h <= 0) return;

    /* SF3000's driver accepts a 640x480 source surface and HCGE performs the
     * final 640->854 panel mapping.  Build the integer-sized game image into
     * that source surface, clearing the unused rows/columns to black first.
     * Keeping the bars in the submitted frame is important: the driver then
     * scales the complete canvas, rather than stretching the game pixels to
     * the panel edges (which made Integer indistinguishable from Native). */
    const int canvas_w = PANEL_PW;
    const int canvas_h = PANEL_PH;
    int nx = canvas_w / w, ny = canvas_h / h;
    int n = nx < ny ? nx : ny;
    if (n < 1) n = 1;
    int env_w = canvas_w, env_h = canvas_h;
    static uint16_t *ib[2]; static int ibi;
    if (!ib[0]) ib[0] = (uint16_t *)malloc(canvas_w * canvas_h * 2);
    if (!ib[1]) ib[1] = (uint16_t *)malloc(canvas_w * canvas_h * 2);
    if (!ib[0] || !ib[1]) return;
    uint16_t *dst = ib[ibi]; ibi ^= 1;
    const uint16_t *s = (const uint16_t *)src;
    int sp = pitch_bytes / 2;
    int active_w = w * n, active_h = h * n;
    /* Oversized core frames are not an Integer use-case, but keep the write
     * bounded if one slips through (n==1 and w>canvas_w). */
    if (active_w > env_w) active_w = env_w;
    if (active_h > env_h) active_h = env_h;
    int ox = (env_w - active_w) / 2;
    int oy = (env_h - active_h) / 2;
    static int last_w = -1, last_h = -1, last_n = -1, last_pitch = -1;
    static unsigned char ib_ready[2];
    /* The two ping-pong canvases retain their black bars between frames.  A
     * full 640x480 clear is therefore needed only when geometry changes;
     * steady-state work is just integer replication of the active rectangle. */
    if (w != last_w || h != last_h || n != last_n || pitch_bytes != last_pitch) {
        ib_ready[0] = ib_ready[1] = 0;
        last_w = w; last_h = h; last_n = n; last_pitch = pitch_bytes;
    }
    if (!ib_ready[ibi ^ 1]) {
        memset(dst, 0, (size_t)canvas_w * canvas_h * sizeof(*dst));
        ib_ready[ibi ^ 1] = 1;
    }
    for (int y = 0; y < h; y++) {
        const uint16_t *sr = s + (size_t)y * sp;
        int copy_w = active_w < w * n ? active_w : w * n;
        int row_y = oy + y * n;
        if (row_y >= env_h) break;
        uint16_t *dr = dst + (size_t)row_y * env_w + ox;
        if (n == 2) {
                /* Two pixels become one 32-bit word each; this is the hot path
                 * for GBA/GBC/NES-sized frames on the 74KC MIPS core. */
                int x = 0, out = 0;
                for (; x + 1 < w && out + 4 <= copy_w; x += 2, out += 4) {
                    uint32_t a = sr[x], b = sr[x + 1];
                    uint32_t *d32 = (uint32_t *)(dr + out);
                    d32[0] = (a << 16) | a;
                    d32[1] = (b << 16) | b;
                }
                for (; x < w && out + 2 <= copy_w; x++, out += 2) {
                    uint32_t px = sr[x];
                    *(uint32_t *)(dr + out) = (px << 16) | px;
                }
        } else {
                int limit = copy_w / n;
                for (int x = 0; x < limit; x++)
                    for (int rx = 0; rx < n; rx++) dr[x * n + rx] = sr[x];
        }
        /* Vertical replication is a bulk copy of the completed row, avoiding
         * repeating the horizontal pixel loop for every duplicate line. */
        size_t row_bytes = (size_t)copy_w * sizeof(uint16_t);
        for (int ry = 1; ry < n && row_y + ry < env_h; ry++)
            memcpy(dst + (size_t)(row_y + ry) * env_w + ox, dr, row_bytes);
    }
    /* Ask HCGE to preserve the 4:3 source canvas while mapping it to the
     * 854x480 panel.  The integer image (including its top/bottom/side bars)
     * remains intact inside that canvas. */
    g_aspect_num = canvas_w;
    g_aspect_den = canvas_h;
    DBG("DBG integer HW: src=%dx%d n=%d active=%dx%d canvas=%dx%d output=854x480\n",
        w, h, n, w * n, h * n, env_w, env_h);

    /* The frame is already panel-sized; keep the driver's normal fit mode. */
    if (p_aspect && g_fs_state != 1) { hwdisp_set_driver_fit(1, 1); g_fs_state = 1; }
    p_disp(dst, env_w, env_h, env_w * 2);
}

void hwdisp_deinit(void) {
    extern void sf3000_dump_fb_state(const char *);
    if (!g_active) { DBG("DBG hwdisp_deinit: not active\n"); return; }
    sf3000_dump_fb_state("hwdisp_deinit/pre");
    /* Clear before teardown as well: the driver may still be scanning its
     * current buffer while p_deinit switches routing back to fb0. */
    clear_fb0_for_shutdown();
    if (p_deinit) p_deinit();
    sf3000_dump_fb_state("hwdisp_deinit/post-p_deinit");
    clear_fb0_for_shutdown();
    if (g_pad_buf) { free(g_pad_buf); g_pad_buf = NULL; g_pad_cap = 0; g_pad_w = 0; g_pad_h = 0; }
    if (g_near_buf) { free(g_near_buf); g_near_buf = NULL; }
    if (g_int_env[0]) { free(g_int_env[0]); g_int_env[0] = NULL; }
    if (g_int_env[1]) { free(g_int_env[1]); g_int_env[1] = NULL; }
    g_int_env_i = 0;
    g_int_env_w[0] = g_int_env_w[1] = 0;
    g_int_env_h[0] = g_int_env_h[1] = 0;
    g_int_src_w[0] = g_int_src_w[1] = 0;
    g_int_src_h[0] = g_int_src_h[1] = 0;
    if (g_handle) { dlclose(g_handle); g_handle = NULL; }
    p_init = NULL; p_deinit = NULL; p_disp = NULL;
    p_g_render = NULL;
    g_active = 0;
    g_native_game_surface = 0;
    g_fs_state = -1;
    g_fs_num = g_fs_den = -1;
    sf3000_dump_fb_state("hwdisp_deinit/post-dlclose");
}
