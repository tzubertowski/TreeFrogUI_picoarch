#include <SDL/SDL.h>
#include <unistd.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <dirent.h>
#include <math.h>
#include "core.h"
#include "libpicofe/fonts.h"
#include "libpicofe/plat.h"
#include "main.h"
#include "menu.h"
#include "plat.h"
#include "scale.h"
#include "scaler_neon.h"
#include "util.h"
#include "libpicofe/in_sdl.h"

static SDL_Surface* screen;

// SF3000 raw framebuffer + input support
#ifdef PLATFORM_SF3000
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include "hwdisp.h"
#include "frogui_settings.h"

int sf3000_use_hwdisp = 0;

/* cubevol shared memory: ftok("/tmp/joy_key", 'a') → 4-byte key state.
   Low 16 bits = button bitmask (bit set = pressed). */
volatile uint32_t *sf3000_keys_ptr = NULL;
/* Filtered key state: rkgame AND cubevol both write joy_key (fork keeps rkgame
 * alive), so the raw value flickers at press/release edges → missed + ghost
 * inputs. The input thread publishes a majority-of-3 filtered value here; ALL
 * readers (menu, game core, hotkeys) use this instead of the raw shm. */
volatile uint32_t sf3000_keys_filtered = 0;

/* Background thread: polls cubevol and injects SDL events so menu works.
   Derives SDL keys from sf3000_keymap[] — no duplicate bit values. */
#include <pthread.h>
static pthread_t sf3000_input_thread;
extern struct sf3000_btn sf3000_keymap[];
extern const int sf3000_keymap_count;

/* Each PHYSICAL button (joy_key bit) emits a FIXED SDL key. libpicofe then maps
 * that SDL key → a retro button via its (remappable) bind table — see
 * in_sdl_defbinds in plat_sf3000.c, whose defaults match these keys exactly. This
 * is the "adapter": the bind table is the remap, edited in OPTION→PLAYER CONTROL
 * and saved to the picoarch config. Every button is surfaced (X/Y/L/R/L2/R2 too),
 * so they can be navigated AND rebound — the old retro_id→key map only covered 7
 * buttons, which is why X/Y couldn't be remapped. */

/* FN physical button support — Feature C1 — R36SX V2.6 CONFIRMED
 * FN is a PHYSICAL input, NOT a retro virtual action. It maps to SDLK_F11
 * (SF3000_FN_SDLKEY) and is UNBOUND by default in in_sdl_defbinds.
 * Raw bit SF3000_FN_BIT=16 (0x00010000) CONFIRMED for R36SX V2.6 via Stock rkgame
 * disassembly (Witali/r36sx_disasm joykey 0x10000) + Stock setting.xml FN+A/B + matching SHA.
 * Other devices remain UNKNOWN (wizard shows FN step, physical bit 16 may never occur, no gating).
 * Visible name "FN" comes from in_sdl_key_names[SDLK_F11] in plat_sf3000.c.
 */
int sf3000_has_fn = 0; /* HAS_FN_BUTTON equivalent: 0=NO/UNKNOWN hidden, 1=YES visible */
static int sf3000_fn_diag = 0; /* diagnostic logging enable */

#ifndef SF3000_FN_BIT
#define SF3000_FN_BIT 16
#endif
#ifndef SF3000_FN_SDLKEY
#define SF3000_FN_SDLKEY SDLK_F11
#endif

static SDLKey sf3000_bit_to_sdlkey(int bit) {
    if (bit == SF3000_FN_BIT) {
        /* FN is data-driven: if bit16 occurs, surface SDLK_F11 / FN. No gating. */
        return SF3000_FN_SDLKEY;
    }
    switch (bit) {
        case 4:  return SDLK_UP;
        case 6:  return SDLK_DOWN;
        case 7:  return SDLK_LEFT;
        case 5:  return SDLK_RIGHT;
        case 13: return SDLK_SPACE;      /* A  → defbind A     */
        case 14: return SDLK_LCTRL;      /* B  → defbind B     */
        case 12: return SDLK_LSHIFT;     /* X  → defbind X     */
        case 15: return SDLK_LALT;       /* Y  → defbind Y     */
        case 10: return SDLK_TAB;        /* L1 → defbind L     */
        case 11: return SDLK_BACKSPACE;  /* R1 → defbind R     */
        case 8:  return SDLK_q;          /* L2 → defbind L2    */
        case 9:  return SDLK_BACKSLASH;  /* R2 → defbind R2    */
        case 0:  return SDLK_RCTRL;      /* SELECT             */
        case 3:  return SDLK_RETURN;     /* START              */
        default: return SDLK_UNKNOWN;
    }
}

/* Device capability init — called from sf3000_keys_init.
 * Final model per C1.9 section 16: FN event path is DATA-DRIVEN, not gated.
 * If bit16 occurs, surface SDLK_F11 / FN. If never occurs, nothing happens.
 * No fn_enable file required. This init only sets sf3000_has_fn for logging/diagnostics;
 * it does NOT suppress valid bit16 events (gating removed). For R36SX V2.6, has_fn will be YES
 * via TF_DEVICE detection, but even if detection fails, FN at bit16 will still be surfaced.
 * Other devices remain NO in log unless env/file override, but event still not suppressed.
 */
static void sf3000_fn_capability_init(void) {
    sf3000_has_fn = 0;
    // Auto-detect R36SX for logging (not for gating)
    const char *tfdev = getenv("TF_DEVICE");
    if (tfdev && strcmp(tfdev, "R36SX") == 0) sf3000_has_fn = 1;
    else {
        FILE *tf = fopen("/tmp/tfdevice.env", "r");
        if (tf) {
            char line[128];
            while (fgets(line, sizeof(line), tf)) {
                if (strstr(line, "TF_DEVICE=R36SX")) { sf3000_has_fn = 1; break; }
            }
            fclose(tf);
        }
    }
    const char *ev = getenv("SF3000_HAS_FN");
    if (ev && (ev[0]=='1' || ev[0]=='y' || ev[0]=='Y')) sf3000_has_fn = 1;
    else if (ev && (ev[0]=='0' || ev[0]=='n' || ev[0]=='N')) sf3000_has_fn = 0;
    FILE *f = fopen("/mnt/sdcard/fn_enable", "r");
    if (f) { char c; if (fread(&c,1,1,f)==1 && (c=='1'||c=='y'||c=='Y')) sf3000_has_fn=1; else if(c=='0'||c=='n'||c=='N') sf3000_has_fn=0; fclose(f); }
#ifdef FN_DIAGNOSTIC
    sf3000_fn_diag = 1;
#else
    const char *dv = getenv("FN_DIAG");
    if (dv && (dv[0]=='1' || dv[0]=='y' || dv[0]=='Y')) sf3000_fn_diag = 1;
    else {
        FILE *df = fopen("/mnt/sdcard/fn_diag", "r");
        if (df) { sf3000_fn_diag = 1; fclose(df); }
    }
#endif
    if (sf3000_has_fn) fprintf(stderr, "SF3000 FN: HAS_FN=YES (bit %d → SDLK_F11 FN, data-driven)\n", SF3000_FN_BIT);
    else fprintf(stderr, "SF3000 FN: HAS_FN=NO (bit %d FN will still surface if pressed, data-driven, diag=%d)\n", SF3000_FN_BIT, sf3000_fn_diag);
}

/* Debounce depth: a bit flips state only after this many consecutive samples
 * agree on the new state. At 8ms/sample, 3 = 24ms (real presses are >50ms). */
#define SF3000_DEBOUNCE_N 3

static void *sf3000_input_thread_fn(void *unused) {
    uint32_t prev = 0;
    uint32_t stable = 0;               /* debounced published state */
    uint8_t  cnt[32] = {0};            /* per-bit consecutive-disagreement count */
    while (1) {
        usleep(8000);                  /* ~8ms sample */
        if (!sf3000_keys_ptr) continue;
        uint32_t raw = *sf3000_keys_ptr;
        /* Per-bit debounce: reject flicker in BOTH directions (press AND release).
         * A bit must hold its new value for N samples before we accept it — this
         * kills the rkgame+cubevol two-writer race AND short analog-stick drift
         * blips, unlike majority-of-3 which let a 2-sample glitch through. */
        for (int b = 0; b < 32; b++) {
            uint32_t m = 1u << b;
            if ((raw & m) != (stable & m)) {
                if (++cnt[b] >= SF3000_DEBOUNCE_N) { stable ^= m; cnt[b] = 0; }
            } else {
                cnt[b] = 0;
            }
        }
        sf3000_keys_filtered = stable;
        uint32_t cur = stable & 0x0001FFFFu;
        uint32_t changed = cur ^ prev;
#ifdef FN_DIAGNOSTIC
        if (changed) {
            uint32_t pressed = cur & changed;
            uint32_t released = (~cur) & changed;
            fprintf(stderr, "FN_DIAG RAW prev=0x%05X cur=0x%05X changed=0x%05X pressed=0x%05X released=0x%05X bits_pressed:",
                    prev & 0x1FFFF, cur, changed, pressed, released);
            for (int b=0;b<17;b++) if (changed & (1u<<b)) fprintf(stderr, " %d", b);
            fprintf(stderr, "\n");
            fflush(stderr);
        }
#else
        if (sf3000_fn_diag && changed) {
            uint32_t pressed = cur & changed;
            uint32_t released = (~cur) & changed;
            fprintf(stderr, "FN_DIAG prev=0x%05X cur=0x%05X changed=0x%05X pressed_bits:",
                    prev & 0x1FFFF, cur, changed);
            for (int b=0;b<17;b++) if (pressed & (1u<<b)) fprintf(stderr, " %d", b);
            fprintf(stderr, " released_bits:");
            for (int b=0;b<17;b++) if (released & (1u<<b)) fprintf(stderr, " %d", b);
            fprintf(stderr, "\n");
        }
#endif
        if (!changed) continue;
        /* Decoupled physical SDL input: process supported raw bits 0..16 via sf3000_bit_to_sdlkey,
         * not limited to sf3000_keymap (which is RetroPad only). FN at bit16 → SDLK_F11. */
        for (int bit = 0; bit <= 16; bit++) {
            uint32_t mask = 1u << bit;
            if (!(changed & mask)) continue;
            SDLKey k = sf3000_bit_to_sdlkey(bit);
            if (k == SDLK_UNKNOWN) continue;
            SDL_Event ev; memset(&ev, 0, sizeof(ev));
            ev.type = (cur & mask) ? SDL_KEYDOWN : SDL_KEYUP;
            ev.key.keysym.sym = k;
            SDL_PushEvent(&ev);
        }
        prev = cur;
    }
    return NULL;
}

static void sf3000_keys_init(void) {
    sf3000_fn_capability_init();
    key_t k = ftok("/tmp/joy_key", 'a');
    if (k == (key_t)-1) { fprintf(stderr, "SF3000 input: ftok failed\n"); return; }
    /* Attach to cubevol's joy_key shm (cubevol reads gpio → writes it). On SF3500
     * rkgame is kept ALIVE (hijack core forks instead of execl) so the input
     * pipeline stays up. IPC_CREAT: harmless if it already exists. */
    int id = shmget(k, 4, IPC_CREAT | 0666);
    if (id < 0) { fprintf(stderr, "SF3000 input: shmget failed errno=%d\n", errno); return; }
    void *p = shmat(id, NULL, 0);
    if (p == (void *)-1) { fprintf(stderr, "SF3000 input: shmat failed\n"); return; }
    sf3000_keys_ptr = (volatile uint32_t *)p;
    fprintf(stderr, "SF3000 input: joy_key shm OK, initial keys=0x%08X\n", *sf3000_keys_ptr);
    pthread_create(&sf3000_input_thread, NULL, sf3000_input_thread_fn, NULL);
}

static int sf3000_fb_fd = -1;
static uint32_t *sf3000_fb_mem = NULL;
static struct fb_var_screeninfo sf3000_vinfo;
static struct fb_fix_screeninfo sf3000_finfo;
static size_t sf3000_fb_size = 0;

// Forward declarations
int sf3000_fb_init(void);
void sf3000_fb_blit(const void *src, int width, int height, int pitch);
void sf3000_fb_finish(void);
int sf3000_is_r36sx(void);
static void sf3000_text_native(int px, int py, const char *text, uint32_t color, int page_y_offset);

#endif

// begin miyoo hardware scaling support
#ifndef PLATFORM_SF3000
// loosely based on eggs' picogpsp gfx.c
#include <mi_sys.h>
#include <mi_gfx.h>
#define	pixelsPa	unused1
#define ALIGN4K(val)	((val+4095)&(~4095))

//
//	Get GFX_ColorFmt from SDL_Surface
//
MI_GFX_ColorFmt_e	GFX_ColorFmt(SDL_Surface *surface) {
	if (surface != NULL) {
		if (surface->format->BytesPerPixel == 2) {
			if (surface->format->Amask == 0) return E_MI_GFX_FMT_RGB565;
			if (surface->format->Amask == 0x8000) return E_MI_GFX_FMT_ARGB1555;
			if (surface->format->Amask == 0xF000) {
				if (surface->format->Bmask == 0x000F) return E_MI_GFX_FMT_ARGB4444;
				return E_MI_GFX_FMT_ABGR4444;
			}
			if (surface->format->Amask == 0x000F) {
				if (surface->format->Bmask == 0x00F0) return E_MI_GFX_FMT_RGBA4444;
				return E_MI_GFX_FMT_BGRA4444;
			}
			return E_MI_GFX_FMT_RGB565;
		}
		if (surface->format->Bmask == 0x000000FF) return E_MI_GFX_FMT_ARGB8888;
		if (surface->format->Amask == 0x000000FF) {
			if (surface->format->Bmask == 0x0000FF00) return E_MI_GFX_FMT_RGBA8888;
			return E_MI_GFX_FMT_BGRA8888;
		}
		if (surface->format->Rmask == 0x000000FF) return E_MI_GFX_FMT_ABGR8888;
	}
	return E_MI_GFX_FMT_ARGB8888;
}

//
//	GFX BlitSurface (MI_GFX ver) / in place of SDL_BlitSurface
//		with scale/bpp convert and rotate/mirror
//		rotate : 1 = 90 / 2 = 180 / 3 = 270
//		mirror : 1 = Horizontal / 2 = Vertical / 3 = Both
//		nowait : 0 = wait until done / 1 = no wait
//
void GFX_BlitSurfaceExec(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect,
			 uint32_t rotate, uint32_t mirror, uint32_t nowait) {
	if ((src != NULL)&&(dst != NULL)&&(src->pixelsPa)&&(dst->pixelsPa)) {
		MI_GFX_Surface_t Src;
		MI_GFX_Surface_t Dst;
		MI_GFX_Rect_t SrcRect;
		MI_GFX_Rect_t DstRect;
		MI_GFX_Opt_t Opt;
		MI_U16 Fence;

		memset(&Opt, 0, sizeof(Opt));
		Opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
		Opt.eRotate = (MI_GFX_Rotate_e)rotate;
		Opt.eMirror = (MI_GFX_Mirror_e)mirror;

		Src.phyAddr = src->pixelsPa;
		Src.u32Width = src->w;
		Src.u32Height = src->h;
		Src.u32Stride = src->pitch;
		Src.eColorFmt = GFX_ColorFmt(src);
		if (srcrect != NULL) {
			SrcRect.s32Xpos = srcrect->x;
			SrcRect.s32Ypos = srcrect->y;
			SrcRect.u32Width = srcrect->w;
			SrcRect.u32Height = srcrect->h;
		} else {
			SrcRect.s32Xpos = 0;
			SrcRect.s32Ypos = 0;
			SrcRect.u32Width = Src.u32Width;
			SrcRect.u32Height = Src.u32Height;
		}

		Dst.phyAddr = dst->pixelsPa;
		Dst.u32Width = dst->w;
		Dst.u32Height = dst->h;
		Dst.u32Stride = dst->pitch;
		Dst.eColorFmt = GFX_ColorFmt(dst);
		if (dstrect != NULL) {
			DstRect.s32Xpos = dstrect->x;
			DstRect.s32Ypos = dstrect->y;
			if ((dstrect->w==0)||(dstrect->h==0)) {
				DstRect.u32Width = SrcRect.u32Width;
				DstRect.u32Height = SrcRect.u32Height;
			} else {
				DstRect.u32Width = dstrect->w;
				DstRect.u32Height = dstrect->h;
			}
		} else {
			DstRect.s32Xpos = 0;
			DstRect.s32Ypos = 0;
			DstRect.u32Width = Dst.u32Width;
			DstRect.u32Height = Dst.u32Height;
		}

		MI_SYS_FlushInvCache(src->pixels, ALIGN4K(src->pitch * src->h));
		MI_SYS_FlushInvCache(dst->pixels, ALIGN4K(dst->pitch * dst->h));
		MI_GFX_BitBlit(&Src, &SrcRect, &Dst, &DstRect, &Opt, &Fence);
		if (!nowait) MI_GFX_WaitAllDone(FALSE, Fence);
	} else SDL_BlitSurface(src, srcrect, dst, dstrect);
}

struct GFX_Buffer {
	MI_PHY		phyAddr;
	void*		virAddr;
	int 		width;
	int 		height;
	int 		depth;
	int 		pitch;
	uint32_t 	size;
};

typedef scale_neon_t scale_func_t;


struct GFX_Scaler {
	scale_func_t upscale;
	
	int src_w;
	int src_h;
	int src_p;
	
	int dst_w;
	int dst_h;
	int dst_p;
	
	int asp_x;
	int asp_y;
	int asp_w;
	int asp_h;
};
static struct GFX_Buffer buffer;
static struct GFX_Scaler scaler;
static SDL_Surface* scaled = NULL;

static uint16_t* dst_buf = NULL;
static size_t dst_buf_p = 0;
static void buffer_upscale(unsigned src_w, unsigned src_h, size_t src_p, const void* src,
			 unsigned dst_w, unsigned dst_h, size_t dst_p, void* dst) {
	
	unsigned scale = dst_w / src_w;
	if (scale==1) {
		memcpy(dst,src,dst_h*dst_p);
		return;
	}
	
	if (dst_p!=dst_buf_p) {
		if (dst_buf) free(dst_buf);
		dst_buf_p = dst_p;
		dst_buf = malloc(dst_p);
	}
	
	unsigned _src_w = src_w;
	unsigned _scale = scale;
	while (src_h) {
		const uint16_t* src_row = src;
		uint16_t* dst_row = dst_buf;
		while (src_w) {
			uint16_t s = *(src_row++);
			while (scale) {
				*(dst_row++) = s;
				scale -= 1;
			}
			scale = _scale;
			src_w -= 1;
		}
		src_w = _src_w;
		
		while (scale) {
			memcpy(dst, dst_buf, dst_p);
			dst += dst_p;
			scale -= 1;
		}
		scale = _scale;
		src_h -= 1;
		
		src += src_p;
	}
}

static void buffer_init(void) {
	buffer.width  = 1280;
	buffer.height =  960;
	buffer.depth  =   16;
	buffer.pitch  = buffer.width * SCREEN_BPP;
	buffer.size = buffer.pitch * buffer.height;

	if (MI_SYS_MMA_Alloc(NULL, ALIGN4K(buffer.size), &buffer.phyAddr)) return;
	MI_SYS_MemsetPa(buffer.phyAddr, 0, buffer.size);
	MI_SYS_Mmap(buffer.phyAddr, ALIGN4K(buffer.size), &buffer.virAddr, TRUE);
	
	PA_INFO("buffer_init()\n"); fflush(stdout);
	
	memset(&scaler, 0, sizeof(struct GFX_Scaler));
}
static void buffer_quit(void) {
	if (buffer.phyAddr) {
		MI_SYS_Munmap(buffer.virAddr, ALIGN4K(buffer.size));
		MI_SYS_MMA_Free(buffer.phyAddr);
	}
	if (scaled) {
		scaled->pixelsPa = 0;
		SDL_FreeSurface(scaled);
		scaled = NULL;
	}
	
	if (dst_buf) free(dst_buf);
}
static void buffer_clear(void) {
	PA_INFO("buffer_clear()\n"); fflush(stdout);
	MI_SYS_FlushInvCache(buffer.virAddr, ALIGN4K(buffer.size));
	MI_SYS_MemsetPa(buffer.phyAddr, 0, buffer.size);
}
static void buffer_renew_surface(int src_w, int src_h, int src_p) {
	if (scaled) {
		PA_INFO("freed %ix%i surface\n", scaled->w, scaled->h); fflush(stdout);
		scaled->pixelsPa = 0;
		SDL_FreeSurface(scaled);
		scaled = NULL;
	}
	
	scaler.src_w = src_w;
	scaler.src_h = src_h;
	scaler.src_p = src_p;
	
	// minimum
	// snes can barely keep up
	int sx = ceilf((float)SCREEN_WIDTH / src_w);
	int sy = ceilf((float)SCREEN_HEIGHT / src_h);
	int s = sx>sy ? sx : sy;
	
	// maximum
	// snes can't keep up (so we added max_upscale)
	// int sx = 2 * SCREEN_WIDTH / src_w;
	// int sy = 2 * SCREEN_HEIGHT / src_h;
	// int s = sx<sy ? sx : sy;
	//
	// if (s>max_upscale) s = max_upscale;
	if (s > max_upscale)
		s = max_upscale;
	if (s < 1)
		s = 1;

	switch (s) {
		case 1: scaler.upscale = scale1x_n16; break;
		case 2: scaler.upscale = scale2x_n16; break;
		case 3: scaler.upscale = scale3x_n16; break;
		case 4: scaler.upscale = scale4x_n16; break;
		case 5: scaler.upscale = scale5x_n16; break;
		case 6: scaler.upscale = scale6x_n16; break;
		// TODO: make an 8x version?
	}

	scaler.dst_w = src_w * s;
	scaler.dst_h = src_h * s;
	scaler.dst_p = scaler.dst_w * SCREEN_BPP;
	
	scaled = SDL_CreateRGBSurfaceFrom(buffer.virAddr,scaler.dst_w,scaler.dst_h,buffer.depth,scaler.dst_p,0,0,0,0);
	if (scaled != NULL) scaled->pixelsPa = buffer.phyAddr;
	PA_INFO("created %ix%i surface (%ix)\n", scaler.dst_w, scaler.dst_h, s); fflush(stdout);
	// buffer_clear();
}
static void buffer_scale(unsigned w, unsigned h, size_t pitch, const void *src) {
	bool update_asp = false;
	// static int scaler_max_upscale;
	if (w!=scaler.src_w || h!=scaler.src_h || pitch!=scaler.src_p) { //  || scaler_max_upscale!=max_upscale
		// scaler_max_upscale = max_upscale;
		buffer_renew_surface(w,h,pitch);
		update_asp = true;
	}
	
	static int scaler_mode;
	if (scaler_mode!=scale_size) {
		scaler_mode = scale_size;
		update_asp = true;
	}
	
	if (update_asp) {
		PA_INFO("update aspect ratio\n"); fflush(stdout);
		buffer_clear();
		
		if (aspect_ratio<=0) aspect_ratio = (float)scaler.src_w / scaler.src_h;
	
		if (scale_size==SCALE_SIZE_ASPECT) {
			scaler.asp_w = SCREEN_HEIGHT * aspect_ratio;
			scaler.asp_h = SCREEN_HEIGHT;
			if (scaler.asp_w>SCREEN_WIDTH) {
				scaler.asp_w = SCREEN_WIDTH;
				scaler.asp_h = SCREEN_WIDTH / aspect_ratio;
			}
		
			scaler.asp_x = (SCREEN_WIDTH - scaler.asp_w) / 2;
			scaler.asp_y = (SCREEN_HEIGHT - scaler.asp_h) / 2;
		}
	}
	
	// buffer_upscale_nn(src);
	// buffer_upscale(scaler.src_w,scaler.src_h,scaler.src_p,src,
	// 		scaler.dst_w,scaler.dst_h,scaler.dst_p,buffer.virAddr);
	scaler.upscale(src, buffer.virAddr,scaler.src_w,scaler.src_h,scaler.src_p,scaler.dst_p);
	
	if (scale_size==SCALE_SIZE_ASPECT) {
		GFX_BlitSurfaceExec(scaled, NULL, screen, &(SDL_Rect){scaler.asp_x, scaler.asp_y, scaler.asp_w, scaler.asp_h},0,0,0);
	}
	else {
		GFX_BlitSurfaceExec(scaled, NULL, screen, NULL,0,0,0);
	}
	
	// just awful
	// void* dst = screen->pixels;
	// if (scale_effect==SCALE_EFFECT_SCANLINE) {
	// 	for (int i=1; i<480; i+=2) {
	// 		memset(dst+i*SCREEN_PITCH, 0, SCREEN_PITCH);
	// 	}
	// }
}

#endif // end miyoo hardware scaling support


//begin PLATFORM_SF3000

// Declare the external event handler from in_sdl.c
//extern void in_sdl_event_handler(void *event_);

// Redirect plat_sdl_event_handler to the external handler
// Stub removed - using proper implementation below instead

struct audio_state {
	unsigned buf_w;
	unsigned max_buf_w;
	unsigned buf_r;
	size_t buf_len;
	struct audio_frame *buf;
	int in_sample_rate;
	int out_sample_rate;
};

struct audio_state audio;

static char msg[HUD_LEN];
static unsigned msg_priority = 0;
static unsigned msg_expire = 0;

// Initialize the framebuffer buffer
int buffer_init(void) {
    buffer.width = SCREEN_WIDTH;
    buffer.height = SCREEN_HEIGHT;
    buffer.depth = SCREEN_BPP;
    buffer.pitch = buffer.width * (SCREEN_BPP / 8);
    buffer.size = buffer.pitch * buffer.height;

    // Allocate memory for the buffer
    buffer.virAddr = malloc(buffer.size);
    if (!buffer.virAddr) {
        PA_ERROR("Failed to allocate memory for framebuffer\n");
        return -1;
    }

    // Clear the buffer
    memset(buffer.virAddr, 0, buffer.size);

    PA_INFO("buffer_init() completed for sf3000\n");
	return 0;
}

// Clean up the framebuffer buffer
void buffer_quit(void) {
    if (buffer.virAddr) {
        free(buffer.virAddr);
        buffer.virAddr = NULL;
    }

    buffer.width = 0;
    buffer.height = 0;
    buffer.depth = 0;
    buffer.pitch = 0;
    buffer.size = 0;

    PA_INFO("buffer_quit() completed for sf3000\n");
}

// Scale the source image to the framebuffer
int buffer_scale(int w, int h, int pitch, const void *data) {
    SDL_Surface *src_surface = SDL_CreateRGBSurfaceFrom(
        (void *)data, w, h, SCREEN_BPP, pitch,
        0x00FF0000,  // Red mask
        0x0000FF00,  // Green mask
        0x000000FF,  // Blue mask
        0xFF000000   // Alpha mask
    );
	return 0;

    if (!src_surface) {
        PA_ERROR("Failed to create source surface: %s\n", SDL_GetError());
        return -1;
    }

    SDL_Rect dst_rect = { 0, 0, buffer.width, buffer.height };
    if (SDL_BlitSurface(src_surface, NULL, screen, &dst_rect) < 0) {
        PA_ERROR("SDL_BlitSurface failed: %s\n", SDL_GetError());
		return -1;
    }

    SDL_FreeSurface(src_surface);

    if (SDL_Flip(screen) < 0) {
        PA_ERROR("SDL_Flip failed: %s\n", SDL_GetError());
		return -1;
    }
}

static void video_expire_msg(void)
{
	msg[0] = '\0';
	msg_priority = 0;
	msg_expire = 0;
}

static void video_update_msg(void)
{
	if (msg[0] && msg_expire < plat_get_ticks_ms())
		video_expire_msg();
}

static void video_clear_msg(uint16_t *dst, uint32_t h, uint32_t pitch)
{
	memset(dst + (h - 10) * pitch, 0, 10 * pitch * sizeof(uint16_t));
}

static void video_print_msg(uint16_t *dst, uint32_t h, uint32_t pitch, char *msg)
{
	basic_text_out16_nf(dst, pitch, 2, h - 10, msg);
}

#ifndef PLATFORM_SF3000
/* Nearest-neighbour resampler for the SDL audio path (non-SF3000 builds). */
static int audio_resample_nearest(struct audio_frame data) {
	static int diff = 0;
	int consumed = 0;

	if (diff < audio.out_sample_rate) {
		audio.buf[audio.buf_w++] = data;
		if (audio.buf_w >= audio.buf_len) audio.buf_w = 0;

		diff += audio.in_sample_rate;
	}

	if (diff >= audio.out_sample_rate) {
		consumed++;
		diff -= audio.out_sample_rate;
	}

	return consumed;
}
#endif

static void *fb_flip(void)
{
#ifdef PLATFORM_SF3000
	/* Only blit during menu — game blits directly in plat_video_process */
	if (screen && g_menuscreen_ptr) {
		extern void sf3000_fb_blit(const void *, int, int, int);
		sf3000_fb_blit(screen->pixels, screen->w, screen->h, screen->pitch);

	}
	return screen ? screen->pixels : NULL;
#else
	SDL_Flip(screen);
	return screen->pixels;
#endif
}

void *plat_prepare_screenshot(int *w, int *h, int *bpp)
{
	if (w) *w = SCREEN_WIDTH;
	if (h) *h = SCREEN_HEIGHT;
	if (bpp) *bpp = SCREEN_BPP;

#ifdef PLATFORM_SF3000
	return screen ? screen->pixels : NULL;
#else
	return screen->pixels;
#endif
}

int plat_dump_screen(const char *filename) {
	char imgname[MAX_PATH];
	int ret = -1;
	SDL_Surface *surface = NULL;

	snprintf(imgname, MAX_PATH, "%s.bmp", filename);

	if (g_menubg_src_ptr) {
		/* Dump the captured menu-bg buffer (the live game frame snapshotted on
		 * menu enter). Not gated on g_menuscreen_ptr: when called straight from
		 * plat_video_menu_enter that pointer isn't set yet, and the SF3000 HW
		 * path never writes screen->pixels, so the else-branch would be black. */
		surface = SDL_CreateRGBSurfaceFrom(g_menubg_src_ptr,
		                                   g_menubg_src_w,
		                                   g_menubg_src_h,
		                                   16,
		                                   g_menubg_src_w * sizeof(uint16_t),
		                                   0xF800, 0x07E0, 0x001F, 0x0000);
		if (surface) {
			ret = SDL_SaveBMP(surface, imgname);
			SDL_FreeSurface(surface);
		}
	} else {
		ret = SDL_SaveBMP(screen, imgname);
	}

	return ret;
}

int plat_load_screen(const char *filename, void *buf, size_t buf_size, int *w, int *h, int *bpp) {
	int ret = -1;
	char imgname[MAX_PATH];
	SDL_Surface *imgsurface = NULL;
	SDL_Surface *surface = NULL;

	snprintf(imgname, MAX_PATH, "%s.bmp", filename);
	imgsurface = SDL_LoadBMP(imgname);
	if (!imgsurface)
		goto finish;

	/* Convert to RGB565 explicitly. SDL_DisplayFormat needs an initialised video
	 * mode (we never SetVideoMode on SF3000), so it would fail or yield 24bpp. */
	{
		SDL_PixelFormat fmt;
		memset(&fmt, 0, sizeof(fmt));
		fmt.BitsPerPixel = 16;
		fmt.BytesPerPixel = 2;
		fmt.Rmask = 0xF800; fmt.Gmask = 0x07E0; fmt.Bmask = 0x001F; fmt.Amask = 0;
		fmt.Rshift = 11; fmt.Gshift = 5; fmt.Bshift = 0;
		fmt.Rloss = 3; fmt.Gloss = 2; fmt.Bloss = 3;
		surface = SDL_ConvertSurface(imgsurface, &fmt, SDL_SWSURFACE);
	}
	if (!surface)
		goto finish;

	if (surface->w == 0 ||
	    (size_t)(surface->h * surface->pitch) > buf_size)
		goto finish;

	memcpy(buf, surface->pixels, surface->pitch * surface->h);
	*w = surface->w;
	*h = surface->h;
	*bpp = 2;

	ret = 0;

finish:
	if (imgsurface)
		SDL_FreeSurface(imgsurface);
	if (surface)
		SDL_FreeSurface(surface);
	return ret;
}


#ifdef PLATFORM_SF3000
/* Last presented frame as RGB565, captured for the menu background + the
 * per-savestate screenshot (the HW path never writes screen->pixels on SF3000). */
static const uint16_t *g_last565 = NULL;
static int g_last565_w = 0, g_last565_h = 0, g_last565_pitch = 0; /* pitch in bytes */
/* Nearest-scale the last presented game frame into the menu-bg buffer so the menu
 * background + saved screenshot (plat_dump_screen reads g_menubg_src_ptr) show the
 * game. The HW path never writes screen->pixels here, so we capture g_last565. */
static void sf3000_capture_menubg(void)
{
	uint16_t *dst = (uint16_t *)g_menubg_src_ptr;
	int dw = g_menubg_src_w, dh = g_menubg_src_h;
	if (!dst) return;
	/* clear to black first so unfilled bars match the game's letterbox */
	memset(dst, 0, (size_t)dh * g_menubg_src_pp * sizeof(uint16_t));
	if (!g_last565 || g_last565_w <= 0 || g_last565_h <= 0)
		return;
	int sw = g_last565_w, sh = g_last565_h, sp = g_last565_pitch / 2;
	/* Match how the game was displayed: FULL stretches to fill; every other
	 * mode keeps the game's aspect (letterbox/pillarbox) so the menu background
	 * lines up with the last gameplay frame for a smooth transition. */
	int ox = 0, oy = 0, ow = dw, oh = dh;
	if (scale_size != SCALE_SIZE_FULL) {
		if ((long)dw * sh > (long)dh * sw) { oh = dh; ow = (int)((long)sw * dh / sh); }
		else                               { ow = dw; oh = (int)((long)sh * dw / sw); }
		if (ow > dw) ow = dw;
		if (oh > dh) oh = dh;
		ox = (dw - ow) / 2; oy = (dh - oh) / 2;
	}
	for (int y = 0; y < oh; y++) {
		int sy = y * sh / oh;
		const uint16_t *srow = g_last565 + (size_t)sy * sp;
		uint16_t *drow = dst + (size_t)(oy + y) * g_menubg_src_pp + ox;
		for (int x = 0; x < ow; x++)
			drow[x] = srow[x * sw / ow];
	}
}
#endif

void plat_video_menu_enter(int is_rom_loaded)
{
	SDL_LockSurface(screen);
#ifdef PLATFORM_SF3000
	sf3000_capture_menubg();
	/* One route for the recents/switcher art: opening the in-game menu updates
	 * the per-game screenshot. g_menubg_src_ptr holds the live frame here, which
	 * is exactly what plat_dump_screen reads. */
	if (is_rom_loaded)
		save_game_screenshot();
	memset(screen->pixels, 0, screen->h * screen->pitch);
#else
	memcpy(g_menubg_src_ptr, screen->pixels, g_menubg_src_h * g_menubg_src_pp * sizeof(uint16_t));
#endif
	SDL_UnlockSurface(screen);
	g_menuscreen_ptr = fb_flip();
}

void plat_video_menu_begin(void)
{
	SDL_LockSurface(screen);
	menu_begin();
}

void plat_video_menu_end(void)
{
	menu_end();
	SDL_UnlockSurface(screen);
	g_menuscreen_ptr = fb_flip();
}

void plat_video_menu_leave(void)
{
	memset(g_menubg_src_ptr, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));

	SDL_LockSurface(screen);
	memset(screen->pixels, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));
	SDL_UnlockSurface(screen);
	fb_flip();
	SDL_LockSurface(screen);
	memset(screen->pixels, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));
	SDL_UnlockSurface(screen);

	g_menuscreen_ptr = NULL;
}

void plat_video_open(void)
{
}

void plat_video_set_msg(const char *new_msg, unsigned priority, unsigned msec)
{
	if (!new_msg) {
		video_expire_msg();
	} else if (priority >= msg_priority) {
		snprintf(msg, HUD_LEN, "%s", new_msg);
		string_truncate(msg, HUD_LEN - 1);
		msg_priority = priority;
		msg_expire = plat_get_ticks_ms() + msec;
	}
}


static SDL_Surface* clean_screen = NULL;
static const void* framebuffer; // NOTE: we don't own this
static int g_game_w = 256, g_game_h = 224; /* actual game dimensions from core */
#ifdef PLATFORM_SF3000
extern int current_pixel_format;
#endif

void* plat_clean_screen(void) {
	return scale_clean(framebuffer, clean_screen->pixels) ? clean_screen : NULL;
}

void plat_video_process(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (!data) return;
	framebuffer = data;
	#ifdef PLATFORM_SF3000
		static unsigned pv_n;
		static unsigned pv_w, pv_h;
		if (pv_n < 24 || width != pv_w || height != pv_h)
			dbg_log("DBG plat_video_process pid=%d n=%u size=%ux%u pitch=%zu fmt=%d\n",
			        getpid(), pv_n + 1, width, height, pitch, current_pixel_format);
		if (pv_n < 24 || width != pv_w || height != pv_h)
			fprintf(stderr, "TFDBG plat_video_process pid=%d n=%u size=%ux%u pitch=%zu fmt=%d\n",
			        getpid(), pv_n + 1, width, height, pitch, current_pixel_format);
		++pv_n; pv_w = width; pv_h = height;
	#endif

	static int had_msg = 0;
	SDL_LockSurface(screen);

	if (had_msg) {
		video_clear_msg(screen->pixels, screen->h, screen->pitch / (SCREEN_BPP / 8));
		had_msg = 0;
	}

#ifdef PLATFORM_SF3000
	extern int current_pixel_format;
	if (current_pixel_format != RETRO_PIXEL_FORMAT_XRGB8888) {
		/* RGB565 / 0RGB1555: blit directly to fb0 (pitch may be wider than width*2). */
		g_game_w = (int)width; g_game_h = (int)height;
		g_last565 = (const uint16_t *)data; g_last565_w = (int)width;
		g_last565_h = (int)height; g_last565_pitch = (int)pitch;
		SDL_UnlockSurface(screen);
		video_update_msg();
		sf3000_fb_blit(data, (int)width, (int)height, (int)pitch);
		return;
	}

	if (current_pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
		/* RGB8888 path: convert to a core-sized RGB565 buffer, then feed the
		 * SAME blit path as native RGB565 cores so hwdisp scales core→panel
		 * (honouring scale modes). The old code center-blitted 1:1 into a
		 * panel-sized buffer, which cropped cores wider than the panel and
		 * defeated scaling. */
		static uint16_t *conv_buf = NULL;
		static size_t conv_cap = 0;
		size_t need = (size_t)width * height;
		if (need > conv_cap) {
			free(conv_buf);
			conv_cap = need + 4096;
			conv_buf = (uint16_t *)malloc(conv_cap * sizeof(uint16_t));
		}
		if (conv_buf) {
			const uint32_t *src32 = (const uint32_t *)data;
			for (unsigned y = 0; y < height; y++) {
				const uint32_t *row = src32 + y * (pitch / 4);
				uint16_t *dst = conv_buf + (size_t)y * width;
				for (unsigned x = 0; x < width; x++) {
					uint32_t p = row[x];
					uint8_t r = (p >> 16) & 0xFF;
					uint8_t g = (p >>  8) & 0xFF;
					uint8_t b = (p)       & 0xFF;
					dst[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
				}
			}
		}
		g_game_w = (int)width; g_game_h = (int)height;
		if (conv_buf) {
			g_last565 = conv_buf; g_last565_w = (int)width;
			g_last565_h = (int)height; g_last565_pitch = (int)width * 2;
		}
		SDL_UnlockSurface(screen);
		video_update_msg();
		if (conv_buf)
			sf3000_fb_blit(conv_buf, (int)width, (int)height, (int)width * 2);
		return;
	}
#endif

	if (scale_size==SCALE_SIZE_NONE) {
		scale(width, height, pitch, data, screen->pixels);
	}
	else {
		buffer_scale(width, height, pitch, data);
	}

	if (msg[0]) {
		video_print_msg(screen->pixels, screen->h, screen->pitch / (SCREEN_BPP / 8), msg);
		had_msg = 1;
	}

	SDL_UnlockSurface(screen);

	video_update_msg();
}

void plat_video_flip(void)
{
	fb_flip();
}

void plat_video_close(void)
{
	
}

unsigned plat_cpu_ticks(void)
{
	long unsigned ticks = 0;
	long ticksps = 0;
	FILE *file = NULL;

	file = fopen("/proc/self/stat", "r");
	if (!file)
		goto finish;

	if (!fscanf(file, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu", &ticks))
		goto finish;

	ticksps = sysconf(_SC_CLK_TCK);

	if (ticksps)
		ticks = ticks * 100 / ticksps;

finish:
	if (file)
		fclose(file);

	return ticks;
}

#ifndef PLATFORM_SF3000
/* SDL audio callback (non-SF3000 builds). */
static void plat_sound_callback(void *unused, uint8_t *stream, int len)
{
	int16_t *p = (int16_t *)stream;
	if (audio.buf_len == 0)
		return;

	len /= (sizeof(int16_t) * 2);

	while (audio.buf_r != audio.buf_w && len > 0) {
		*p++ = audio.buf[audio.buf_r].left;
		*p++ = audio.buf[audio.buf_r].right;
		audio.max_buf_w = audio.buf_r;

		len--;
		audio.buf_r++;

		if (audio.buf_r >= audio.buf_len) audio.buf_r = 0;
	}

	while(len > 0) {
		*p++ = 0;
		--len;
	}
}
#endif

#ifdef PLATFORM_SF3000
static void *sf3000_sound_handle = NULL;
static int (*sf3000_sound_driver_init)(void *device_name, int sample_rate, int channels) = NULL;
static int (*sf3000_sound_driver_playframe)(const void *buffer, int bytes) = NULL;
static int (*sf3000_sound_driver_deinit)(void) = NULL;

/* SW output gain workaround for the stock linear volume curve (too loud at low
 * settings; there is no global volume knob on this hardware). The level is a
 * percent 0..100, editable in the in-game menu (Audio and video → Volume) and
 * persisted to cubegm/sndgain.txt; also re-read on each game launch. */
int  sf3000_snd_gain_pct = 100;          /* menu edits this (extern in menu.c) */
static int sf3000_snd_gain_q8 = 256;     /* 8.8 fixed derived value (256 = 1.0) */
#define SF3000_SNDGAIN_PATH "/mnt/sdcard/cubegm/sndgain.txt"

/* cubevol's live I2SO volume control, reverse-engineered from its
 * api_set_volume().  Software-zeroed PCM still leaves the DAC/amp live and
 * can hiss, so Volume 0 must also silence this hardware path. */
#define SF3000_I2SO_SET_VOLUME 0x8001080bu
static void sf3000_i2so_set_volume(int level)
{
	unsigned char value;
	int fd;
	if (level < 0) level = 0;
	if (level > 100) level = 100;
	value = (unsigned char)level;
	fd = open("/dev/sndC0i2so", O_WRONLY);
	if (fd < 0) return;
	(void)ioctl(fd, SF3000_I2SO_SET_VOLUME, &value);
	close(fd);
}

/* ---- Shared system volume (cubevol persistentmem) --------------------------
 * The console volume lives in ONE place: cubevol's avparam blob in
 * /dev/persistentmem (req {flag=3,id=0,len=260}, volume = buf[0], 0..100).
 * That is what the PHYSICAL volume buttons read/write (cubevol also mirrors
 * it to the I2SO hardware path on every press).  Making it the single source
 * of truth links Settings' Volume slider, the in-game menu and the physical
 * buttons into ONE volume - changing it anywhere is visible everywhere in
 * real time.  Structure + ioctls RE'd from cubevol's avparam_get/save_volume
 * (same API family as the backlight slot; see FrogUI's backlight.c).
 * sndgain.txt is kept as a legacy fallback only (missing pmem / old root). */
struct sf3000_pmem_req { unsigned short flag, id, len, pad; void *buf; };
#define SF3000_PMEM_GET 0x400c2602u
#define SF3000_PMEM_SET 0x800c2603u

static int sf3000_pmem_volume_read(void)
{
	unsigned char buf[260];
	int fd = open("/dev/persistentmem", O_RDONLY);
	if (fd >= 0) {
		struct sf3000_pmem_req req = { 3, 0, 260, 0, buf };
		if (ioctl(fd, SF3000_PMEM_GET, &req) == 0 && buf[0] <= 100) {
			close(fd);
			return buf[0];
		}
		close(fd);
	}
	return -1;
}

/* Store the shared volume AND mirror it to the I2SO hardware path, exactly
 * like a cubevol button press does.  Also refresh sndgain.txt so legacy
 * standalone frontends (pcsx4all, lgpt) still see a consistent level.
 * persistentmem is EEPROM-like: compare first and skip the durable write
 * when the level is unchanged - this path is hit by in-menu volume steps
 * and the live poll, where rewriting the same value adds wear and latency
 * for no effect.
 * The SET is byte-exact with cubevol's avparam_save_volume (RE'd from the
 * stock daemon): the volume node {flag=3,id=0} is written with len=1 - a
 * single byte.  A 260-byte blob SET never reaches the node cubevol reads
 * (the physical volume meter then ignores our writes). */
static void sf3000_pmem_volume_write(int level)
{
	unsigned char byte;
	int fd;
	if (level < 0) level = 0;
	if (level > 100) level = 100;
	if (sf3000_pmem_volume_read() == level)
		goto mirror;   /* already stored: hardware mirror only, no EEPROM write */
	byte = (unsigned char)level;
	fd = open("/dev/persistentmem", O_RDWR);
	if (fd >= 0) {
		struct sf3000_pmem_req req = { 3, 0, 1, 0, &byte };
		(void)ioctl(fd, SF3000_PMEM_SET, &req);
		close(fd);
	}
mirror:
	sf3000_i2so_set_volume(level);
	FILE *f = fopen(SF3000_SNDGAIN_PATH, "w");
	if (f) { fprintf(f, "%d\n", level); fclose(f); }
}


/* Speaker-amp mute line (idle-static fix): the console mutes its speaker
 * amplifier through GPIO_L pad /panel/speaker-output, driven with INVERTED
 * polarity from what the DT name suggests.  Ground truth captured by
 * watching the GPIO bank while plugging/unplugging headphones:
 *   - idle, amp hissing:  pad LOW
 *   - headphones in, speaker dead: pad HIGH (and it stays HIGH while the
 *     plug is in; LOW again on unplug, hiss returns)
 * So HIGH = speaker muted, LOW = speaker live.  Holding the pad HIGH while
 * no real audio flows removes the constant menu/static hiss at the physical
 * source - it survives every digital mute because the amp itself is the
 * noise source.  The pad is configured OUTPUT first (the kernel boot leaves
 * it input; cubevol's gpio_configure does the same at daemon start).
 *
 * BOARD BACKEND, FAIL CLOSED: the raw /dev/mem mapping is only driven when
 * the running device + its live device-tree EXACTLY match one validated
 * board entry below.  Any mismatch (unknown device, absent/renamed DT node,
 * pin out of range for that board) leaves the gate disabled: every caller
 * becomes a no-op and the amp stays in its stock state, so an unvalidated
 * target can never have unrelated registers touched.  Devices with no
 * validated entry (SF3000/GB350/SF3500 today) are stock-by-construction. */
int sf3000_device_id(void);   /* TF_DEV_* (defined with device detection) */
enum { TF_DEV_SF3000 = 0, TF_DEV_R36SX = 1, TF_DEV_SF3500 = 2, TF_DEV_GB350 = 3 };
static volatile uint32_t *sf3000_gpioL_regs = NULL;
static int sf3000_mute_pin = -1;
static int sf3000_spk_mute_state = -1;   /* -1 unknown, 0 live, 1 muted */
static int sf3000_gate_enabled = -1;     /* -1 = read cubegm config once */
static pthread_mutex_t sf3000_spk_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Validated board table.  dt_node      - the device-tree property holding
 * the mute pad (big-endian u32 pin number); mute_pol - 1 when HIGH means
 * muted.  Entries exist ONLY for boards whose full DT/pin/register mapping
 * was physically verified (ground truth: AMPMON captures, headphone
 * plug/unplug bank diffs).  Adding a device requires capturing its own
 * evidence first, never extending an existing entry's ranges. */
struct sf3000_spk_board {
	int dev_id;             /* TF_DEV_* (plat_sdl device detection) */
	const char *dt_node;    /* expected DT node under /proc/device-tree */
	int pin_min, pin_max;  /* validated pin range for this board */
	int mute_pol;           /* 1: HIGH = muted (R36SX measured) */
};
static const struct sf3000_spk_board sf3000_spk_boards[] = {
	/* R36SX V2.6: /panel/speaker-output = GPIO_L pin, HIGH mutes the amp.
	 * Captured on-device while plugging/unplugging headphones. */
	{ TF_DEV_R36SX, "/proc/device-tree/panel/speaker-output", 0, 31, 1 },
};
#define SF3000_SPK_BOARD_N ((int)(sizeof(sf3000_spk_boards) / sizeof(sf3000_spk_boards[0])))

/* Persistent opt-out, in the same style as the other cubegm/ flags:
 * an empty or absent cubegm/spk_gate.txt keeps the speaker gate ON (the
 * fix for the constant idle static); a file containing "off" restores the
 * stock always-live amp.  The gate never touches anything else - sounds
 * (menu ticks, games, apps) simply open the line while they have content. */
static int sf3000_gate_config(void)
{
	if (sf3000_gate_enabled < 0) {
		sf3000_gate_enabled = 1;
		FILE *f = fopen("/mnt/sdcard/cubegm/spk_gate.txt", "r");
		if (f) {
			char b[8] = {0};
			if (fgets(b, sizeof b, f) && strncmp(b, "off", 3) == 0)
				sf3000_gate_enabled = 0;
			fclose(f);
		}
		if (sf3000_gate_enabled)
			dbg_log("DBG A: speaker gate ON (cubegm/spk_gate.txt default)\n");
		else
			dbg_log("DBG A: speaker gate OFF by cubegm/spk_gate.txt\n");
	}
	return sf3000_gate_enabled;
}

/* Resolve the mute pad for the RUNNING device and check it against the
 * validated board table.  Returns the matching entry, or NULL when this
 * device/pin combination was never validated (gate stays disabled). */
static const struct sf3000_spk_board *sf3000_spk_board_match(void)
{
	int id = sf3000_device_id();   /* TF_DEV_* for the running console */
	for (int i = 0; i < SF3000_SPK_BOARD_N; i++)
		if (sf3000_spk_boards[i].dev_id == id)
			return &sf3000_spk_boards[i];
	return NULL;
}

/* Map the GPIO_L bank and resolve the mute pad, but ONLY for a validated
 * board: the running device must match a table entry, the DT node that
 * entry declares must exist, and the pin it reports must be inside the
 * entry's validated range.  Anything else fails closed (gate disabled,
 * stock amp behaviour) - /dev/mem is never touched on an unvalidated
 * target.  Returns 0 when the pad is drivable, non-zero otherwise. */
static int sf3000_spk_mute_map(void)
{
	const struct sf3000_spk_board *board;
	if (!sf3000_gate_config()) return -1;   /* user opted out */
	if (sf3000_gpioL_regs) return 0;
	board = sf3000_spk_board_match();
	if (!board) {
		dbg_log("DBG A: speaker gate: no validated board for this device "
		         "- staying stock (fail closed)\n");
		return -1;
	}
	if (sf3000_mute_pin < 0) {
		char path[96];
		char buf[16];
		uint32_t v;
		int n, fd;
		snprintf(path, sizeof path, "%s", board->dt_node);
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			/* DT node missing/renamed on this firmware: mapping not
			 * validated for it - fail closed. */
			dbg_log("DBG A: speaker gate: DT node %s absent - fail closed\n",
			        board->dt_node);
			return -1;
		}
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n < 4) { sf3000_mute_pin = -2; return -1; }
		memcpy(&v, buf, 4);
		v = ntohl(v);   /* DT cells are big-endian */
		/* The DT pin must land inside THIS board's validated range. */
		if (v < (uint32_t)board->pin_min || v > (uint32_t)board->pin_max) {
			sf3000_mute_pin = -2;
			dbg_log("DBG A: speaker gate: DT pin %u outside validated range "
			        "%d..%d - fail closed\n",
			        (unsigned)v, board->pin_min, board->pin_max);
			return -1;
		}
		sf3000_mute_pin = (int)v;
	}
	{
		int fd = open("/dev/mem", O_RDWR | O_SYNC);
		void *m;
		if (fd < 0) return -1;
		m = mmap(NULL, 0x2020, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		         0x18800000);
		close(fd);
		if (m == MAP_FAILED) return -1;
		sf3000_gpioL_regs = (volatile uint32_t *)((char *)m + 0x44);
	}
	return 0;
}

/* Drive the speaker mute line: mute=1 -> pad HIGH (speaker silent),
 * mute=0 -> pad LOW (speaker live).  All state and GPIO transitions are
 * serialized: the audio thread's gate, the pause-menu re-assert and the
 * exec/teardown paths can race on this - one mutex keeps the amp from
 * being left stuck muted or live by interleaved opens/closes. */
static void sf3000_spk_mute(int mute)
{
	uint32_t bit;
	pthread_mutex_lock(&sf3000_spk_mtx);
	if (sf3000_spk_mute_map() != 0) {
		pthread_mutex_unlock(&sf3000_spk_mtx);
		return;
	}
	bit = 1u << sf3000_mute_pin;
	/* direction (+0x14): OUTPUT before driving the value */
	if (!(sf3000_gpioL_regs[5] & bit))
		sf3000_gpioL_regs[5] |= bit;
	if (mute) {
		if (sf3000_spk_mute_state != 1) {
			sf3000_gpioL_regs[4] |= bit;    /* +0x10: HIGH = muted */
			sf3000_spk_mute_state = 1;
		}
	} else {
		if (sf3000_spk_mute_state != 0) {
			sf3000_gpioL_regs[4] &= ~bit;  /* LOW = live */
			sf3000_spk_mute_state = 0;
		}
	}
	pthread_mutex_unlock(&sf3000_spk_mtx);
}

/* How long the speaker stays live after the last REAL audio chunk was
 * consumed.  Core bursts re-arm it continuously while a game plays music;
 * the grace also spans short emulation gaps without audible gating.  Once
 * it expires with nothing but concealment/idle, the amp line mutes and the
 * idle hiss is gone until the next sound. */
#define SF3000_SPK_LIVE_GRACE_MS 300

/* Launcher (FrogUI) hold-open window after the last real audio chunk.  The
 * amp itself needs a few ms of live line before it reproduces anything, so
 * closing the line the instant a discrete tick's content is spent swallowed
 * the whole blip (silent menu ticks even though the samples reached the
 * DAC).  While the window is live the consumer keeps FEEDING DIGITAL
 * SILENCE.  30 ms = the minimum drain (3 x 10 ms chunks) that lets the DAC
 * consume what is in flight without truncating the click - then the gate
 * closes the line and the launcher returns to the validated silent-idle
 * state.  Field tuning (R36SX): the earlier 500 ms keep-warm window kept
 * the DAC/amp powered after every tick and its residual SNR floor was
 * audible as a short static tail; shortening to the drain window removes
 * it.  Slow navigation stays audible on the FIRST press because a closed
 * line re-arms via the 100 ms silence runway (SF3000_TICK_ARM_CHUNKS) -
 * the arm-then-play path that was validated for cold ticks. */
#define SF3000_TICK_HOLD_MS 30

/* Amp arm-up: when the first real content arrives with the speaker line
 * still closed, open it and feed this many 10 ms chunks of digital silence
 * BEFORE consuming the tick.  The amp and the firmware's playback pipeline
 * need the runway (the old instant playback died inside the amp ramp-up);
 * digital silence primes them exactly like a game's audio path without
 * any hiss.  100 ms total. */
#define SF3000_TICK_ARM_CHUNKS 10

static uint64_t sf3000_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Silence-base speaker gate - the "silence layer" over the whole OS: the amp
 * line is HIGH (muted) by default everywhere.  It opens (LOW) only around
 * real audio content, and closes again the moment the content is gone - a
 * discrete launcher tick opens it for exactly the tick, a continuous game
 * soundtrack keeps it open via the grace window that spans core burst gaps.
 * grace_ms == 0 closes with zero open-but-silent window (discrete sounds);
 * games pass SF3000_SPK_LIVE_GRACE_MS so chunk-to-chunk gaps never click. */
static void sf3000_spk_live_gate(uint64_t last_real_us, unsigned grace_ms)
{
	int want_live = last_real_us != 0 && grace_ms > 0 &&
		(sf3000_now_us() - last_real_us) <
			(uint64_t)grace_ms * 1000u;
	if (want_live && sf3000_spk_mute_state != 0) {
		sf3000_spk_mute(0);
		if (sf3000_spk_mute_state == 0)
			dbg_log("DBG A: audio flows; speaker line LOW\n");
	} else if (!want_live && sf3000_spk_mute_state != 1) {
		sf3000_spk_mute(1);
		dbg_log("DBG A: audio idle; speaker line HIGH (silent)\n");
	}
}

/* Recompute the live gain from the percent and persist it. Called from the
 * in-game menu.  Writes the SHARED system volume (cubevol's persistentmem
 * slot + I2SO hardware + legacy sndgain.txt), so the in-game Volume slider
 * and the physical volume buttons are one and the same value. */
void sf3000_apply_snd_gain(void) {
	if (sf3000_snd_gain_pct < 0)   sf3000_snd_gain_pct = 0;
	if (sf3000_snd_gain_pct > 100) sf3000_snd_gain_pct = 100;
	sf3000_snd_gain_q8 = sf3000_snd_gain_pct * 256 / 100;
	sf3000_pmem_volume_write(sf3000_snd_gain_pct);
}

/* Read the shared system volume.  Cubevol's persistentmem slot (the physical
 * buttons' value) wins; legacy sndgain.txt is only consulted when the pmem
 * slot is unreadable (old root, pmem failure). */
static void sf3000_load_snd_gain(void) {
	int p = sf3000_pmem_volume_read();
	if (p < 0) {
		p = 100;
		FILE *f = fopen(SF3000_SNDGAIN_PATH, "r");
		if (f) { int v; if (fscanf(f, "%d", &v) == 1) p = v; fclose(f); }
	}
	if (p < 0)   p = 0;
	if (p > 100) p = 100;
	sf3000_snd_gain_pct = p;
	sf3000_snd_gain_q8 = p * 256 / 100;
}

/* Live volume reload for the running session.  The physical volume buttons
 * (cubevol) change the persistentmem slot out from under us at any moment -
 * poll it (see the audio thread's periodic check) and follow the shared
 * value in real time, both the hardware path and our SW gain.  This is what
 * makes a button press audible in the running game without a reboot, and
 * keeps FrogUI's slider in sync with the buttons. */
static void sf3000_reload_snd_gain_live(void) {
	int p = sf3000_pmem_volume_read();
	if (p < 0) return;
	if (p != sf3000_snd_gain_pct) {
		sf3000_snd_gain_pct = p;
		sf3000_snd_gain_q8 = p * 256 / 100;
		/* mirror to the hardware volume path exactly like a cubevol press
		 * (re-asserting is harmless and covers paths that bypassed it) */
		sf3000_i2so_set_volume(p);
		dbg_log("DBG A: live volume -> %d%% (pmem, hw+sw)\n", p);
	}
}

/* Non-blocking audio: a dedicated consumer thread owns the *blocking*
 * sound_driver_playframe() DAC write. The emu thread only enqueues into this
 * SPSC ring and never blocks, so audio over/underrun can never freeze the
 * emulator. Video stays the sole frame pacer. */
#define SF3000_ARING_FRAMES 4096          /* power of two, ~85ms @ 48000Hz */
#define SF3000_ARING_MASK   (SF3000_ARING_FRAMES - 1)
#define SF3000_ACHUNK       480           /* 10ms at the fixed 48kHz DAC rate */
#define SF3000_APREFILL     1440          /* 30ms cushion for expensive scaled frames */

static struct audio_frame sf3000_aring[SF3000_ARING_FRAMES];
static unsigned           sf3000_aring_w = 0;   /* producer: emu thread   */
static unsigned           sf3000_aring_r = 0;   /* consumer: audio thread */
static unsigned           sf3000_underruns = 0;
static unsigned           sf3000_overruns = 0;
static pthread_mutex_t    sf3000_aring_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t          sf3000_audio_thread;
static volatile int       sf3000_audio_running = 0;
static volatile int       sf3000_audio_menu_paused = 0;
static volatile int       sf3000_audio_resume_immediate = 0;
static uint32_t           sf3000_rs_phase = 0;  /* linear-resample phase, 16.16 */
static struct audio_frame sf3000_rs_prev;       /* previous input frame */

static volatile int sf3000_audio_init_rc = 0;   /* 0=pending, 1=ok, -1=failed */

/* AUDDEC/I2SO can survive a process transition in a half-alive state: init
 * reports success, but no samples reach the speaker until another application
 * (notably pcsx4all) opens and closes the driver. Keep every operation on the
 * audio thread because SF3500-class drivers store their handle thread-locally.
 * A clean cycle reproduces the known PSX recovery automatically at startup. */
static int sf3000_audio_driver_start(int clean_cycle)
{
	if (!sf3000_sound_driver_init)
		return 0;

	for (int pass = 0; pass < (clean_cycle ? 2 : 1); pass++) {
		int rc = -1;
		for (int try = 0; try < 20; try++) {
			rc = sf3000_sound_driver_init(NULL, SAMPLE_RATE, 2);
			if (rc >= 0)
				break;
			usleep(100 * 1000);
		}
		if (rc < 0)
			return -1;

		if (pass == 0 && clean_cycle) {
			dbg_log("DBG A: clean AUDDEC/I2SO startup cycle\n");
			if (sf3000_sound_driver_deinit)
				sf3000_sound_driver_deinit();
			/* The firmware audio daemon releases the devices asynchronously. */
			usleep(100 * 1000);
		}
	}
	return 0;
}

static void *sf3000_audio_thread_fn(void *unused)
{
	(void)unused;
	struct audio_frame chunk[SF3000_ACHUNK];
	uint64_t next_us = 0;
	uint64_t last_real_us = 0;   /* last consumed REAL audio (MONOTONIC) */
	int primed = 0;
	int driver_active = 0;
	int recovering = 0;
	int arm_chunks = 0;   /* launcher: silence runway before the first tick */
	/* LAUNCHER MODE: g_is_frogui IS the single authoritative value (set once
	 * in main() before plat_init, never mutated after).  Reading it here
	 * directly - no copied launcher flag that could drift from the real
	 * process state (review: one authoritative mode/state value). */
	extern int g_is_frogui;
#define launcher (g_is_frogui)
	unsigned gain_reload_polls = 0;	struct audio_frame last = { 0, 0 };

	/* Silence-base speaker gate: mute the amp line before any driver
	 * startup - the pre-audio loading window would hiss otherwise.  The
	 * line goes live with the first real audio chunk (grace gate below). */
	sf3000_spk_mute(1);

	/* Init the DAC ON THIS THREAD. The SF3500/HD/SF3100 driver keeps its audio
	 * handle thread-local, so sound_driver_init() and sound_driver_playframe()
	 * MUST run on the same thread — init on the emu thread left a NULL handle
	 * here, so playframe deref'd it (+0x278 SIGSEGV). pcsx4all works precisely
	 * because it inits + feeds on one thread. Doing it here fixes SF3500-class
	 * and is harmless for SF3000 (same-thread init is strictly safer).
	 * RETRY on failure: at menu→game transitions the previous process's
	 * deinit settles asynchronously in the audio daemon; a first-try init can
	 * fail and giving up left the whole system silent until reboot. */
	/* A previous core can leave AUDDEC/I2SO busy for considerably longer than
	 * the normal 100 ms settle window. Keep the consumer alive and retry rather
	 * than permanently silencing the session; this is the same recovery users
	 * previously triggered by launching and exiting PCSX4ALL. */
	while (sf3000_audio_running && sf3000_audio_driver_start(1) < 0) {
		PA_ERROR("SF3000: sound_driver_init failed; retrying audio startup\n");
		dbg_log("DBG A: sound_driver_init failed; retry in 250ms\n");
		sf3000_audio_init_rc = -1;
		usleep(250 * 1000);
	}
	if (!sf3000_audio_running)
		return NULL;
	driver_active = 1;
	sf3000_audio_init_rc = 1;

	while (sf3000_audio_running) {
		int have_chunk = 0;
		/* Live Volume (Settings slider): re-read sndgain.txt ~once a second
		 * so a level change applies without a reboot.  Games get it through
		 * their pause menu too; this covers the launcher and in-game. */
		if (++gain_reload_polls >= 500) {
			gain_reload_polls = 0;
			sf3000_reload_snd_gain_live();
		}
		if (sf3000_audio_menu_paused) {
			pthread_mutex_lock(&sf3000_aring_mtx);
			sf3000_aring_r = sf3000_aring_w;
			pthread_mutex_unlock(&sf3000_aring_mtx);
			primed = 0;
			next_us = 0;
			recovering = 0;
			last.left = last.right = 0;
			/* Pause menu: nothing plays here, and the user expects instant
			 * silence on an explicit pause - skip the grace and mute right
			 * away, re-asserted every cycle. resume_from_menu re-arms the
			 * gate through last_real_us on the first game chunk. */
			sf3000_spk_mute(1);
			usleep(2000);
			continue;
		}

		pthread_mutex_lock(&sf3000_aring_mtx);
		unsigned avail = sf3000_aring_w - sf3000_aring_r;  /* free-running */
		if (sf3000_audio_resume_immediate) {
			/* The menu deliberately flushed the queue.  Do not force playback
			 * from that empty queue; wait for the normal prefill below. */
			sf3000_audio_resume_immediate = 0;
		}
		/* Libretro cores submit audio in one-video-frame bursts. Buffer 30ms
		 * before starting, then feed the non-blocking stock driver at a steady
		 * 48kHz instead of inheriting that burst cadence. A scaled frame can be
		 * a few milliseconds late on these CPUs, so do not turn the first short
		 * poll into a full re-prime (and an audible 20ms gap).
		 * LAUNCHER: prefill is ZERO - a navigation tick is a short discrete
		 * burst (observed arriving as ~98-frame ring fragments) and must play
		 * on FIRST press.  ARM-THEN-PLAY: the first content with the amp line
		 * still closed opens it and feeds SF3000_TICK_ARM_CHUNKS of digital
		 * silence first - the amp/pipeline need that runway or the tick dies
		 * inside the ramp-up (field-tested: only every 2nd fast press was
		 * audible).  While content keeps arriving (hold window) the DAC stays
		 * fed with silence, keeping the pipeline hot for the next press. */
		if (!primed && avail >= (launcher ? 1u : SF3000_APREFILL)) {
			if (launcher) {
				/* At system volume 0 (sndgain 0) the content is inaudible by
				 * design - keep the line closed; nothing needs the amp. */
				if (sf3000_snd_gain_q8 > 0) {
					int was_muted = (sf3000_spk_mute_state != 0);
					sf3000_spk_mute(0);
					if (was_muted) {
						/* cold-start: arm the amp with silence before the tick */
						arm_chunks = SF3000_TICK_ARM_CHUNKS;
						dbg_log("DBG A: launcher cold tick; arming %u x10ms\n",
						        (unsigned)SF3000_TICK_ARM_CHUNKS);
					}
				}
			}
			primed = 1;
		}
		if (primed) {
			unsigned take = avail < SF3000_ACHUNK ? avail : SF3000_ACHUNK;
			/* LAUNCHER ARM PHASE: the amp line just opened for this tick.
			 * Consume DIGITAL SILENCE chunks first (real content stays
			 * queued behind them) so the amp and the firmware pipeline are
			 * fully live before the tick plays - that is what makes the
			 * FIRST press audible.  Identical to a running game's silence,
			 * so it cannot hiss. */
			if (launcher && arm_chunks > 0) {
				arm_chunks--;
				for (int i = 0; i < SF3000_ACHUNK; i++) {
					chunk[i].left = 0;
					chunk[i].right = 0;
				}
				last.left = last.right = 0;
				have_chunk = 1;
				/* real audio is queued; treat it as flowing for the gate */
				if (take)
					last_real_us = sf3000_now_us();
				pthread_mutex_unlock(&sf3000_aring_mtx);
				goto play_silence;
			}
			/* LAUNCHER: a tick is a discrete event.  Once its content is
			 * spent (take==0, nothing but concealment would follow), drop
			 * the primed state and fall to the idle path - the hold window
			 * below keeps feeding silence while it lasts, then the gate
			 * closes the amp line.  The next tick re-primes (and re-opens
			 * the line) on its own press.  Games keep the stock concealment
			 * cadence (anti-crackle). */
			if (launcher && take == 0) {
				primed = 0;
				next_us = 0;
				recovering = 0;
				pthread_mutex_unlock(&sf3000_aring_mtx);
				continue;
			}
			if (take)
				last_real_us = sf3000_now_us();   /* real audio flowed */
			for (unsigned i = 0; i < take; i++) {
				chunk[i] = sf3000_aring[sf3000_aring_r & SF3000_ARING_MASK];
				sf3000_aring_r++;
			}
			/* Blend from the preceding concealed tail into newly available audio
			 * instead of jumping straight to an arbitrary sample.  For a long stall
			 * `last` is zero; for a short miss it is the held waveform sample. */
			if (recovering && take) {
				unsigned fade = take < 64 ? take : 64;
				for (unsigned i = 0; i < fade; i++) {
					int32_t ldelta = (int32_t)chunk[i].left - last.left;
					int32_t rdelta = (int32_t)chunk[i].right - last.right;
					chunk[i].left = (int16_t)(last.left + ldelta * (i + 1) / fade);
					chunk[i].right = (int16_t)(last.right + rdelta * (i + 1) / fade);
				}
				recovering = 0;
			}
		/* Never starve the proprietary driver once playback has started.
		 * Stopping writes until a full re-prime made a brief scheduling miss
		 * turn into a 30ms hole, and restarting on an arbitrary waveform edge
		 * produced the reported crackle.  Preserve cadence and ramp the missing
		 * tail to zero instead.  Usually this conceals only a handful of frames.
		 * LAUNCHER: a tick is DISCRETE - what follows its last sample is
		 * digital silence by design, so pad any shortage with pure zeros.
		 * Holding the last edge here (fine for games) played a short burst
		 * of the tick's tail amplitude at the end of each blip - the brief
		 * static heard on-device after every menu tick. */
		if (take < SF3000_ACHUNK) {
			unsigned missing = SF3000_ACHUNK - take;
			if (launcher) {
				for (unsigned i = 0; i < missing; i++) {
					chunk[take + i].left = 0;
					chunk[take + i].right = 0;
				}
			} else {
				struct audio_frame from = take ? chunk[take - 1] : last;
				/* Tiny SNES shortages (often just 2--60 frames) must not be
				 * amplified into a full fade-to-zero.  Hold the edge for up to
				 * 3.3ms; reserve silence fading for a substantial stall. */
				if (missing <= 160) {
					for (unsigned i = 0; i < missing; i++)
						chunk[take + i] = from;
				} else {
					for (unsigned i = 0; i < missing; i++) {
						unsigned remain = missing - i - 1;
						chunk[take + i].left = (int16_t)((int32_t)from.left * remain / missing);
						chunk[take + i].right = (int16_t)((int32_t)from.right * remain / missing);
					}
				}
			}
			if (++sf3000_underruns <= 8 ||
			    !(sf3000_underruns & (sf3000_underruns - 1)))
				dbg_log("DBG A: underrun #%u: had %u/%u frames (overruns=%u)\n",
				        sf3000_underruns, take, SF3000_ACHUNK,
				        sf3000_overruns);
			recovering = 1;
		}
			last = chunk[SF3000_ACHUNK - 1];
			have_chunk = 1;
		}
		pthread_mutex_unlock(&sf3000_aring_mtx);

		if (!have_chunk) {
			/* Loading stall / silent gap.  Games: the gate closes once real
			 * audio stops flowing (grace window spans emulation bursts).
			 * LAUNCHER HOLD: while the hold window after the last real tick
			 * is still live, keep FEEDING DIGITAL SILENCE instead of idling -
			 * the amp and the firmware pipeline stay hot, so the next tick
			 * (even a slow single press) is audible immediately, and the
			 * fed silence is exactly what a running game emits during quiet
			 * passages, so it cannot hiss.  Once the hold expires the gate
			 * closes the line and the launcher returns to the validated
			 * silent-idle state (no hiss at rest). */
			if (launcher &&
			    sf3000_snd_gain_q8 > 0 &&
			    last_real_us != 0 &&
			    (sf3000_now_us() - last_real_us) <
			        (uint64_t)SF3000_TICK_HOLD_MS * 1000u) {
				for (int i = 0; i < SF3000_ACHUNK; i++) {
					chunk[i].left = 0;
					chunk[i].right = 0;
				}
				last.left = last.right = 0;
				/* fall through to the driver write below (skip gate + gain:
				 * zeros stay zeros; the pause-menu path above handles its
				 * own mute) */
			} else {
				sf3000_spk_live_gate(last_real_us,
				                     launcher ? SF3000_TICK_HOLD_MS
				                                              : SF3000_SPK_LIVE_GRACE_MS);
				usleep(2000);  /* underrun: wait for the emu thread to refill */
				continue;
			}
		} else if (!launcher) {
			sf3000_spk_live_gate(last_real_us, SF3000_SPK_LIVE_GRACE_MS);
		}
play_silence:

		/* Software output gain (volume-curve workaround). The stock level→volume
		 * mapping is linear, so low system-volume settings stay loud. Scaling our
		 * own output down lets the user run the system volume higher, making the
		 * low end actually quiet. q8 fixed-point; 256 = 1.0 = unchanged. */
		if (sf3000_snd_gain_q8 < 256) {
			for (int i = 0; i < SF3000_ACHUNK; i++) {
				chunk[i].left  = (int16_t)(((int)chunk[i].left  * sf3000_snd_gain_q8) >> 8);
				chunk[i].right = (int16_t)(((int)chunk[i].right * sf3000_snd_gain_q8) >> 8);
			}
		}

		/* SF3000-class handhelds have one physical speaker.  Feeding the stock
		 * driver hard-panned stereo can therefore lose whichever channel is not
		 * wired to that speaker (very obvious in Mega Drive music).  Downmix with
		 * 32-bit accumulation to avoid overflow, then feed mono to both lanes. */
		if (audio_mono) {
			for (int i = 0; i < SF3000_ACHUNK; i++) {
				/* Arithmetic shift is cheaper than division on this MIPS target.
				 * The one-bit negative rounding difference is inaudible. */
				int16_t mono = (int16_t)(((int32_t)chunk[i].left + chunk[i].right) >> 1);
				chunk[i].left = chunk[i].right = mono;
			}
		}

		/* Some stock driver variants return immediately while others block.
		 * Clock the non-blocking case at exactly chunk/48000 seconds; if the
		 * call already blocked, the deadline has passed and no sleep is added. */
		int play_rc = sf3000_sound_driver_playframe
			? sf3000_sound_driver_playframe(chunk, SF3000_ACHUNK) : -1;
		if (play_rc < 0) {
			/* A failed write means the firmware lost AUDDEC/I2SO ownership.
			 * Reopen it here, on its owner thread, then refill from a clean ring. */
			dbg_log("DBG A: playframe failed rc=%d; recovering audio driver\n", play_rc);
			if (sf3000_sound_driver_deinit)
				sf3000_sound_driver_deinit();
			driver_active = 0;
			usleep(100 * 1000);
			while (sf3000_audio_running && sf3000_audio_driver_start(0) < 0) {
				dbg_log("DBG A: runtime audio recovery failed; retry in 250ms\n");
				sf3000_audio_init_rc = -1;
				usleep(250 * 1000);
			}
			if (!sf3000_audio_running)
				break;
			driver_active = 1;
			pthread_mutex_lock(&sf3000_aring_mtx);
			sf3000_aring_r = sf3000_aring_w;
			pthread_mutex_unlock(&sf3000_aring_mtx);
			primed = 0;
			next_us = 0;
			recovering = 0;
			last.left = last.right = 0;
			dbg_log("DBG A: runtime audio recovery OK\n");
			continue;
		}
		{
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			uint64_t now = (uint64_t)ts.tv_sec * 1000000u +
			               (uint64_t)ts.tv_nsec / 1000u;
			if (!next_us) next_us = now;
			next_us += (uint64_t)SF3000_ACHUNK * 1000000u / SAMPLE_RATE;
			if (next_us > now) {
				uint64_t delay = next_us - now;
				if (delay > 20000u) delay = 20000u;
				usleep((useconds_t)delay);
			} else if (now - next_us > 20000u) {
				next_us = now;
			}
		}
	}
	/* Deinit on the same thread that init'd + played (thread-local handle). */
	if (driver_active && sf3000_sound_driver_deinit)
		sf3000_sound_driver_deinit();
	return NULL;
#undef launcher
}
#endif

static void plat_sound_finish(void)
{
#ifdef PLATFORM_SF3000
	if (sf3000_audio_running) {
		sf3000_audio_running = 0;
		pthread_join(sf3000_audio_thread, NULL);  /* thread deinits the DAC itself */
	}
	if (sf3000_sound_handle) {
		dlclose(sf3000_sound_handle);
		sf3000_sound_handle = NULL;
	}
#else
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	if (audio.buf) {
		free(audio.buf);
		audio.buf = NULL;
	}
#endif
}

#ifdef PLATFORM_SF3000
/* Standalone media players need AUDDEC/I2SO released before exec, but calling
 * all of plat_finish() also tears down SDL/fb0 state that the firmware loader
 * expects to survive the handoff. Keep this deliberately audio-only.
 * The amp line is NOT closed here: the next process (libffplayer video/image
 * viewer, or FrogUI via the fallback in quit()) re-inits the whole digital
 * audio path itself, so the physical line must stay OPEN for it to be
 * audible - the caller opens or closes it as appropriate. */
void sf3000_sound_finish_for_exec(void)
{
	plat_sound_finish();
}

/* Standalone apps (PCSX, DOS, Rockbox, media players) open their OWN audio
 * and have no speaker gate - they need the amp line OPEN to be audible.
 * Called before exec-ing one. */
void sf3000_sound_open_for_exec(void)
{
	sf3000_spk_mute(0);
}

/* Close (mute) the speaker-amp line: the game→FrogUI handoff - FrogUI
 * (v1.3.0_i) never powers the DAC, so the next process inherits silence
 * instead of a live, hissing amp. */
void sf3000_sound_close_for_exec(void)
{
	sf3000_spk_mute(1);
}

/* Standalone handoff, audio side A (see main.c quit()): stop picoarch's
 * audio consumer thread - the thread deinits the proprietary DAC itself,
 * the only thread-safe way to release AUDDEC/I2SO (the SF3500-class driver
 * keeps its handle thread-local).  Keeps driver.so dlopen'ed so the next
 * picoarch process finds the same library state as before.  The amp line
 * is NOT touched here; main.c opens it as the very last step before exec.
 *
 * Bounded retry around the ACTUAL driver release (no fixed sleep): the
 * firmware's audio daemon releases AUDDEC/I2SO asynchronously (~100 ms
 * observed) after a deinit, and exposes no pollable status - the ONLY
 * ready condition available is a real sound_driver_init() probe, which
 * fails while the daemon still holds the async release and succeeds once
 * it has completed (the same fact picoarch's own startup retry relies
 * on).  Two probe phases, each exiting the moment the daemon confirms:
 *   1. confirm the release of the consumer thread's deinit;
 *   2. the probe handle itself is released (thread-local driver), whose
 *      own async release is confirmed in turn.
 * The exec'd app therefore inherits a FULLY settled audio state - the
 * same guarantee the old fixed 150 ms sleep gave, but each phase takes
 * exactly as long as the daemon actually needs.  Apps without their own
 * init retry (rockbox, libffplayer) lose the race otherwise; pcsx4all
 * survived only because it retries.  Each phase is bounded (30 x 10 ms,
 * ~3x the observed settle); on a timeout we proceed as the old code
 * would have, never blocking the handoff forever. */
static int sf3000_probe_driver_release(void)
{
	for (int tries = 0; tries < 30; tries++) {
		if (sf3000_sound_driver_init(NULL, SAMPLE_RATE, 2) >= 0)
			return tries;   /* daemon confirmed the release */
		usleep(10 * 1000);
	}
	return -1;   /* bounded: give up, behave like the old fixed wait */
}

void sf3000_sound_release_for_exec(void)
{
	if (sf3000_audio_running) {
		sf3000_audio_running = 0;
		pthread_join(sf3000_audio_thread, NULL);  /* thread deinits the DAC */
	}
	if (!sf3000_sound_driver_init || !sf3000_sound_driver_deinit)
		return;   /* driver never loaded: nothing of ours is held */
	int t1 = sf3000_probe_driver_release();
	if (t1 >= 0) {
		/* Release confirmed.  Free the probe handle (on this thread - the
		 * driver handle is thread-local) and confirm that second release
		 * too, so the next app starts from a fully settled daemon. */
		sf3000_sound_driver_deinit();
		int t2 = sf3000_probe_driver_release();
		dbg_log("DBG A: exec handoff: releases confirmed (%d+%d probes)\n",
		        t1, t2 >= 0 ? t2 : 30);
	} else {
		dbg_log("DBG A: exec handoff: release probe hit the bound; "
		        "continuing unconfirmed (old fixed-wait behaviour)\n");
	}
}
#endif

const char *sf3000_driver_path(void);

static int plat_sound_init(void)
{
#ifdef PLATFORM_SF3000
	/* NOTE: sound_driver_init() is NOT called here — it runs on the audio thread
	 * (the SF3500-class driver's handle is thread-local; see sf3000_audio_thread_fn).
	 * Here we only dlopen/dlsym, set rates, and start that thread. */
	if (!sf3000_sound_handle) {
		sf3000_sound_handle = dlopen(sf3000_driver_path(), RTLD_LAZY);
		if (!sf3000_sound_handle)   /* fall back to generic driver.so */
			sf3000_sound_handle = dlopen("/mnt/sdcard/cubegm/driver.so", RTLD_LAZY);
		if (!sf3000_sound_handle) {
			PA_ERROR("SF3000: Failed to load driver.so for audio: %s\n", dlerror());
			return -1;
		}

		sf3000_sound_driver_init = dlsym(sf3000_sound_handle, "sound_driver_init");
		sf3000_sound_driver_playframe = dlsym(sf3000_sound_handle, "sound_driver_playframe");
		sf3000_sound_driver_deinit = dlsym(sf3000_sound_handle, "sound_driver_deinit");

		if (!sf3000_sound_driver_init || !sf3000_sound_driver_playframe) {
			PA_ERROR("SF3000: Missing audio driver functions in driver.so\n");
			plat_sound_finish();
			return -1;
		}
	}

	audio.in_sample_rate = sample_rate;
	audio.out_sample_rate = SAMPLE_RATE;

	/* Start the non-blocking audio consumer thread (it init's the DAC itself). */
	sf3000_load_snd_gain();   /* re-read cubegm/sndgain.txt each game launch */
	/* The frontend volume setting is normally a software gain so it preserves
	 * the stock hardware curve.  At exactly zero, also close the analogue path:
	 * otherwise a "muted" game can still emit DAC/amp white noise. FrogUI
	 * restores the saved hardware level before every non-muted game launch. */
	if (sf3000_snd_gain_pct == 0)
		sf3000_i2so_set_volume(0);
	sf3000_aring_w = sf3000_aring_r = 0;
	sf3000_underruns = sf3000_overruns = 0;
	sf3000_rs_phase = 0;
	sf3000_rs_prev.left = sf3000_rs_prev.right = 0;
	sf3000_audio_init_rc = 0;
	if (!sf3000_audio_running) {
		sf3000_audio_running = 1;
		if (pthread_create(&sf3000_audio_thread, NULL,
		                   sf3000_audio_thread_fn, NULL) != 0) {
			sf3000_audio_running = 0;
			PA_ERROR("SF3000: failed to start audio thread\n");
			return -1;
		}
	}

	PA_INFO("SF3000: Proprietary audio driver initialized at %d Hz\n", SAMPLE_RATE);
	return 0;
#else
	if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		return -1;
	}

	SDL_AudioSpec spec, received;

	spec.freq = SAMPLE_RATE;
	spec.format = AUDIO_S16;
	spec.channels = 2;
	spec.samples = 512;
	spec.callback = plat_sound_callback;

	if (SDL_OpenAudio(&spec, &received) < 0) {
		plat_sound_finish();
		return -1;
	}

	audio.in_sample_rate = sample_rate;
	audio.out_sample_rate = received.freq;
	plat_sound_resize_buffer();

	SDL_PauseAudio(0);
	return 0;
#endif
}

float plat_sound_capacity(void)
{
#ifdef PLATFORM_SF3000
	return 1.0;
#else
	int buffered = 0;
	if (audio.buf_len == 0)
		return 1.0;

	if (audio.buf_w != audio.buf_r) {
		buffered = audio.buf_w > audio.buf_r ?
			audio.buf_w - audio.buf_r :
			(audio.buf_w + audio.buf_len) - audio.buf_r;
	}

	return 1.0 - (float)buffered / audio.buf_len;
#endif
}

#define BATCH_SIZE 100
void plat_sound_write(const struct audio_frame *data, int frames)
{
#ifdef PLATFORM_SF3000
	/* Non-blocking enqueue: hand frames to the consumer thread and return
	 * immediately. On overrun we drop the excess rather than block the emu.
	 * The DAC is fixed at SAMPLE_RATE (48kHz); cores emitting any other rate
	 * (Genesis 44.1kHz, SNES 32040Hz...) are linear-resampled here, else they
	 * play pitch-shifted and starve the ring. Nearest (dup/drop) crackled —
	 * duplicating every other sample at SNES's 1.5x ratio is audible ZOH
	 * distortion. 48kHz cores (e.g. nestopia) land on frac≈0 → passthrough. */
	if (sf3000_audio_running) {
		int in_rate = audio.in_sample_rate > 0
			? audio.in_sample_rate : audio.out_sample_rate;
		pthread_mutex_lock(&sf3000_aring_mtx);
		/* Phase step per output sample, 16.16 fixed: in_rate/out_rate.
		 * The emulation and DAC clocks are independent and differ slightly on
		 * real units (F-Zero drained a few dozen frames every few seconds despite
		 * nominally exact rates).  Nudge the existing resampler by only 1/512
		 * according to queue occupancy.  This keeps the ring centred without an
		 * extra operation in the per-sample loop or an audible pitch change. */
		uint32_t step = (uint32_t)(((uint64_t)in_rate << 16) / audio.out_sample_rate);
		unsigned queued = sf3000_aring_w - sf3000_aring_r;
		if (queued < SF3000_ARING_FRAMES / 2)
			step -= step >> 9; /* low queue: stretch by ~0.2% */
		else if (queued > SF3000_ARING_FRAMES * 3 / 4)
			step += step >> 9; /* high queue: contract by ~0.2% */
		for (int i = 0; i < frames; i++) {
			struct audio_frame cur = data[i];
			/* emit outputs positioned between prev and cur */
			while (sf3000_rs_phase < 0x10000) {
				int frac = sf3000_rs_phase & 0xffff;
				struct audio_frame out;
				if (sf3000_aring_w - sf3000_aring_r >= SF3000_ARING_FRAMES) {
					/* Keep the newest audio.  Retaining stale queued samples after a
					 * slow frame adds latency and eventually makes the discontinuity
					 * worse when the remainder of this batch is dropped. */
					sf3000_aring_r += SF3000_ACHUNK;
					if (++sf3000_overruns <= 8 ||
					    !(sf3000_overruns & (sf3000_overruns - 1)))
						dbg_log("DBG A: overrun #%u; dropped %u oldest frames\n",
						        sf3000_overruns, (unsigned)SF3000_ACHUNK);
				}
				/* delta*frac>>16 can exceed int16 (delta up to ±65535) — keep the
				 * math in int32; the interpolated result itself always lies
				 * between prev and cur, so only the final value fits int16. */
				out.left  = (int16_t)(sf3000_rs_prev.left +
					(int32_t)(((int64_t)(cur.left  - sf3000_rs_prev.left)  * frac) >> 16));
				out.right = (int16_t)(sf3000_rs_prev.right +
					(int32_t)(((int64_t)(cur.right - sf3000_rs_prev.right) * frac) >> 16));
				sf3000_aring[sf3000_aring_w & SF3000_ARING_MASK] = out;
				sf3000_aring_w++;
				sf3000_rs_phase += step;
			}
			sf3000_rs_phase -= 0x10000;
			sf3000_rs_prev = cur;
		}
		pthread_mutex_unlock(&sf3000_aring_mtx);
	}
#else
	int consumed = 0;
	if (audio.buf_len == 0)
		return;

	SDL_LockAudio();

	while (frames > 0) {
		int tries = 0;
		int amount = MIN(BATCH_SIZE, frames);

		while (tries < 10 && audio.buf_w == audio.max_buf_w) {
			tries++;
			SDL_UnlockAudio();

			if (!limit_frames)
				return;

			SDL_Delay(1);
			SDL_LockAudio();
		}

		while (amount && audio.buf_w != audio.max_buf_w) {
			consumed = audio_resample_nearest(*data);
			data += consumed;
			amount -= consumed;
			frames -= consumed;
		}
	}
	SDL_UnlockAudio();
#endif
}

void plat_sound_resize_buffer(void) {
#ifdef PLATFORM_SF3000
	return;
#else
	size_t buf_size;
	SDL_LockAudio();

	audio.buf_len = frame_rate > 0
		? current_audio_buffer_size * audio.in_sample_rate / frame_rate
		: 0;

	if (audio.buf_len == 0) {
		SDL_UnlockAudio();
		return;
	}

	buf_size = audio.buf_len * sizeof(struct audio_frame);
	audio.buf = realloc(audio.buf, buf_size);

	if (!audio.buf) {
		SDL_UnlockAudio();
		PA_ERROR("Error initializing sound buffer\n");
		plat_sound_finish();
		return;
	}

	memset(audio.buf, 0, buf_size);
	audio.buf_w = 0;
	audio.buf_r = 0;
	audio.max_buf_w = audio.buf_len - 1;
	SDL_UnlockAudio();
#endif
}

void plat_sound_pause_for_menu(void)
{
#ifdef PLATFORM_SF3000
	sf3000_audio_menu_paused = 1;
	pthread_mutex_lock(&sf3000_aring_mtx);
	sf3000_aring_r = sf3000_aring_w;
	pthread_mutex_unlock(&sf3000_aring_mtx);
#endif
}

void plat_sound_resume_from_menu(void)
{
#ifdef PLATFORM_SF3000
	sf3000_audio_resume_immediate = 1;
	sf3000_audio_menu_paused = 0;
#endif
}

void plat_sdl_event_handler(void *event_)
{
    SDL_Event *event = (SDL_Event *)event_;

    switch (event->type) {
        case SDL_QUIT:
            exit(0);
            break;

        default:
            break;
    }
}

int plat_init(void)
{
#ifdef PLATFORM_SF3000
    setenv("SDL_NOMOUSE", "1", 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        PA_ERROR("SF3000 SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    // Wire display controller to fb0 AFTER SDL_Init so SDL can't reset it
    extern int sf3000_fb_init(void);
    if (sf3000_fb_init() < 0) {
        PA_ERROR("sf3000_fb_init failed\n");
        return -1;
    }
    dbg_log("DBG M1: fb_init ok\n");
    /* Off-screen staging buffer only — do NOT call SDL_SetVideoMode,
       it changes fb0 resolution away from 720x1280 and breaks our mmap writes. */
    screen = SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_WIDTH, SCREEN_HEIGHT, 16,
                                  0xF800, 0x07E0, 0x001F, 0x0000);
    if (!screen) {
        PA_ERROR("SF3000 SDL_CreateRGBSurface failed: %s\n", SDL_GetError());
        return -1;
    }

    fprintf(stderr, "SDL surface: %dx%d bpp=%d pitch=%d\n",
            screen->w, screen->h, screen->format->BitsPerPixel, screen->pitch);
    PA_INFO("SF3000: fb0 + SDL dummy surface ready (%dx%d)\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    SDL_ShowCursor(0);
    dbg_log("DBG M2: sdl surface ok\n");

    g_menuscreen_w  = SCREEN_WIDTH;
    g_menuscreen_h  = SCREEN_HEIGHT;
    g_menuscreen_pp = SCREEN_WIDTH;
    g_menuscreen_ptr = NULL;
    g_menubg_src_w  = SCREEN_WIDTH;
    g_menubg_src_h  = SCREEN_HEIGHT;
    g_menubg_src_pp = SCREEN_WIDTH;

    if (in_sdl_init(&in_sdl_platform_data, plat_sdl_event_handler)) {
        PA_ERROR("SF3000 SDL input failed to init: %s\n", SDL_GetError());
        return -1;
    }
    in_probe();

    /* FrogUI is a launcher.  Do not power the proprietary DAC for it: the
       analogue output can hiss even with no samples and a UI volume of zero.
       Exception: FrogUI's own Settings can enable navigation ticks
       (menu_sounds=on).  With ticks on, the launcher needs the audio path -
       the speaker gate keeps every other window silent, so the DAC is only
       powered when the user asked for audible UI feedback.
       Games run in a fresh PicoArch exec and initialise audio normally.
       The settings file is read through the ONE shared parser
       (frogui_settings.h) - no duplicated FrogUI parsing here. */
    extern int g_is_frogui;
    if (g_is_frogui) {
        int launcher_audio = frogui_setting_is("menu_sounds", "on");
        if (launcher_audio) {
            if (plat_sound_init() == 0) {
                /* path exists but starts silent: nothing plays until a tick */
                sf3000_spk_mute(1);
                PA_INFO("SF3000: launcher audio enabled (menu ticks on)\n");
            }
        } else {
            /* No DAC for the launcher (v1.3.0_i design) - and make sure the
             * amp line is muted: a standalone app (PCSX etc.) exits with the
             * line open and its DAC closed, which is exactly the amp-hiss
             * state. The launcher mutes it on arrival, every arrival. */
            sf3000_spk_mute(1);
            PA_INFO("SF3000: audio DAC disabled for FrogUI launcher\n");
        }
    } else if (plat_sound_init()) {
        PA_ERROR("SF3000 SDL sound failed to init (continuing without audio): %s\n", SDL_GetError());
    }

dbg_log("DBG M3: sound init done\n");
    sf3000_keys_init();
    extern void sf3000_calibrate_input(void);
    extern void sf3000_load_keymap(void);
    /* Load saved mapping if exists, else run one-time calibration */
    {
        /* Keymap is hardcoded and confirmed. Write file so calibration
           never runs. Delete /mnt/sdcard/sf3000_keymap.txt to recalibrate. */
        {
            static const char *_n[] = {
                "UP","DOWN","LEFT","RIGHT","A","B","X","Y",
                "L","R","L2","R2","SELECT","START" };
            FILE *_kf = fopen("/mnt/sdcard/sf3000_keymap.txt", "r");
            if (_kf) { fclose(_kf); sf3000_load_keymap(); }
            else {
                FILE *_wf = fopen("/mnt/sdcard/sf3000_keymap.txt", "w");
                if (_wf) {
                    for (int _i = 0; _i < sf3000_keymap_count; _i++)
                        fprintf(_wf, "%s %d\n", _n[_i], sf3000_keymap[_i].bit);
                    fclose(_wf);
                }
            }
        }
    }

    return 0;
#else
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        PA_ERROR("%s, SDL_Init failed: %s\n", __func__, SDL_GetError());
        return -1;
    }
	screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, 0, SDL_SWSURFACE | SDL_DOUBLEBUF);
	PA_INFO("Using Framebuffer: %s\n", getenv("SDL_FBDEV"));
	if (screen == NULL) {
		PA_ERROR("%s, failed to set video mode\n", __func__, SDL_GetError());
		return -1;
	}
	
	clean_screen = SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_WIDTH, SCREEN_HEIGHT, 0,
                                    0x00FF0000,  // Red mask
                                    0x0000FF00,  // Green mask
                                    0x000000FF,  // Blue mask
                                    0xFF000000); // Alpha mask
	if (clean_screen == NULL) {
		PA_ERROR("%s, SDL_CreateRGBSurface failed: %s\n", __func__, SDL_GetError());
		return -1;
	}
	
	buffer_init();
	
	SDL_ShowCursor(0);

	g_menuscreen_w = SCREEN_WIDTH;
	g_menuscreen_h = SCREEN_HEIGHT;
	g_menuscreen_pp = SCREEN_WIDTH;
	g_menuscreen_ptr = NULL;
	g_menubg_src_w = SCREEN_WIDTH;
	g_menubg_src_h = SCREEN_HEIGHT;
	g_menubg_src_pp = SCREEN_WIDTH;

	if (in_sdl_init(&in_sdl_platform_data, plat_sdl_event_handler)) {
		PA_ERROR("SDL input failed to init: %s\n", SDL_GetError());
		return -1;
	}
	in_probe();

	if (plat_sound_init()) {
		PA_ERROR("SDL sound failed to init: %s\n", SDL_GetError());
		return -1;
	}
	printf("Framebuffer resolution: %dx%d, color depth: %d bpp\n", SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_BPP * 8);
	return 0;
#endif // PLATFORM_SF3000
}

int plat_reinit(void)
{
#ifdef PLATFORM_SF3000
	/* The DAC stays at the fixed 48kHz it was init'd with on the audio thread;
	 * plat_sound_write nearest-resamples each core's native rate to it. Do NOT
	 * deinit/re-init the driver here: this runs on the emu thread, and when the
	 * audio thread is blocked inside sound_driver_playframe (UAE's load-time
	 * warmup pre-fills the ring) a concurrent deinit crashes the driver. The
	 * SF3500-class driver's thread-local handle forbids cross-thread init too.
	 * A fixed 48kHz DAC also skips the driver's own (poor) resampler, which is
	 * what made non-48kHz cores sound bad in the first place. */
	audio.in_sample_rate = sample_rate;
	DBG("DBG R: plat_reinit in_rate=%d out_rate=%d\n",
	    audio.in_sample_rate, audio.out_sample_rate);
#else
	audio.in_sample_rate = sample_rate;
	plat_sound_resize_buffer();
#endif
	scale_update_scaler();
	return 0;
}

void plat_finish(void)
{
#ifdef PLATFORM_SF3000
	extern void sf3000_fb_finish(void);
	plat_sound_finish();
	sf3000_fb_finish();
	SDL_FreeSurface(screen);
	screen = NULL;
	SDL_Quit();
#else
	plat_sound_finish();
	buffer_quit();
	SDL_FreeSurface(clean_screen);
	SDL_FreeSurface(screen);
	screen = NULL;
	SDL_Quit();
#endif
}

// SF3000 framebuffer implementation
#ifdef PLATFORM_SF3000

/* Phase-A diagnostic: dump state of fb0/fb6/fb22 at any moment so we can
 * see which layers driver.so leaves enabled and whether display routing
 * stays on fb0 or shifts to a HW layer. */
void sf3000_dump_fb_state(const char *tag) {
    const int devs[] = {0, 6, 22};
    for (size_t i = 0; i < sizeof(devs)/sizeof(devs[0]); i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/fb%d", devs[i]);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            DBG("DBG dump[%s] %s: open failed (errno=%d)\n", tag, path, errno);
            continue;
        }
        struct fb_var_screeninfo v;
        struct fb_fix_screeninfo f;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 &&
            ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0) {
            DBG("DBG dump[%s] %s: %ux%u(v%ux%u) bpp=%u off=%u,%u rot=%u "
                            "line=%u smem_start=0x%lx smem_len=%u type=%u visual=%u\n",
                    tag, path,
                    v.xres, v.yres, v.xres_virtual, v.yres_virtual,
                    v.bits_per_pixel, v.xoffset, v.yoffset, v.rotate,
                    f.line_length, (unsigned long)f.smem_start, f.smem_len,
                    f.type, f.visual);
        } else {
            DBG("DBG dump[%s] %s: ioctl failed (errno=%d)\n", tag, path, errno);
        }
        close(fd);
    }
}

int sf3000_fb_init(void) {
    /* Detect device + export panel geometry to env before the core's retro_init
     * reads it (single binary → SF3000 854x480 or R36SX 640x480). */
    { extern void sf3000_detect_device(void); sf3000_detect_device(); }

    /* SF-class devices are hardware-present only. The stock/decrypted driver
     * owns the portrait framebuffer geometry and HCGE performs the panel
     * transform/scaling. Do not touch fb0 with FBIOPUT, mmap it, or fall back
     * to the legacy software transpose path. */
    if (!sf3000_is_r36sx()) {
        extern int hwdisp_init(void);
        if (hwdisp_init() == 0) {
            sf3000_use_hwdisp = 1;
            /* Driver set fb geometry+rotation. Turn the display ON (rkgame did this
             * at boot; we killed rkgame) WITHOUT FBIOPUT (that clobbers geometry).
             * Wire display→fb0 + unblank. */
            int dis = open("/dev/dis", O_RDWR);
            if (dis >= 0) { struct { int a,b,c; } b = {1,0,0}; ioctl(dis, 0xc00c0e0c, &b); close(dis); }
            int fb = open("/dev/fb0", O_RDWR);
            if (fb >= 0) { ioctl(fb, FBIOBLANK, 1); ioctl(fb, FBIOBLANK, 0); close(fb); }
            fprintf(stderr, "sf3000_fb_init: SF-class hardware display active (disp_frame only)\n");
            return 0;
        }
        fprintf(stderr, "sf3000_fb_init: SF-class hwdisp_init failed; software framebuffer disabled\n");
        return -1;
    }
    /* Write per-process init log to dedicated file with fsync, so it
     * survives even when stderr buffer is lost on power-cycle. */
    int initlog = open("/mnt/sdcard/picoarch_init.log",
                       O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (initlog >= 0) {
        dprintf(initlog, "\n=== picoarch init pid=%d ppid=%d ===\n",
                getpid(), getppid());
    }
    /* Dump kernel-side fb/display info once on each startup so we can see
     * what devices exist, what panel state the kernel reports, etc. */
    {
        const char *paths[] = {"/proc/fb", "/proc/iomem", "/proc/interrupts", NULL};
        for (int i = 0; paths[i]; i++) {
            FILE *pf = fopen(paths[i], "r");
            if (!pf) { DBG("DBG %s: open failed\n", paths[i]); continue; }
            char buf[512];
            int line_count = 0;
            while (fgets(buf, sizeof(buf), pf) && line_count++ < 40) {
                size_t n = strlen(buf);
                if (n && buf[n-1] == '\n') buf[n-1] = 0;
                /* Only log lines that look relevant (fb / disp / panel / hcge). */
                if (strstr(buf, "fb") || strstr(buf, "isp") ||
                    strstr(buf, "anel") || strstr(buf, "cge") ||
                    strstr(buf, "lcd") || strstr(buf, "/dev/fb") ||
                    i == 0) /* /proc/fb is small, dump fully */
                    DBG("DBG %s: %s\n", paths[i], buf);
            }
            fclose(pf);
        }
    }
    /* Wire display controller to fb0 — must happen AFTER SDL_Init so SDL
       can't reset this connection. Extracted from fbdev_init() disassembly. */
    int dis = open("/dev/dis", O_RDWR);
    if (dis >= 0) {
        struct { int a, b, c; } buf = {1, 0, 0};
        int r = ioctl(dis, 0xc00c0e0c, &buf);
        fprintf(stderr, "SF3000: /dev/dis ioctl ret=%d\n", r);
        close(dis);
    } else {
        fprintf(stderr, "SF3000: /dev/dis open failed\n");
    }

    // Open framebuffer (720x1280, 32-bit ARGB)
    sf3000_fb_fd = open("/dev/fb0", O_RDWR);
    if (sf3000_fb_fd < 0) return -1;

    ioctl(sf3000_fb_fd, FBIOGET_VSCREENINFO, &sf3000_vinfo);
    DBG("DBG sf3000_fb_init: PRE-FBIOPUT %ux%u(v%ux%u) bpp=%u off=%u,%u\n",
            sf3000_vinfo.xres, sf3000_vinfo.yres,
            sf3000_vinfo.xres_virtual, sf3000_vinfo.yres_virtual,
            sf3000_vinfo.bits_per_pixel,
            sf3000_vinfo.xoffset, sf3000_vinfo.yoffset);
    if (initlog >= 0) {
        dprintf(initlog, "PRE-FBIOPUT %ux%u(v%ux%u) bpp=%u off=%u,%u rot=%u\n",
                sf3000_vinfo.xres, sf3000_vinfo.yres,
                sf3000_vinfo.xres_virtual, sf3000_vinfo.yres_virtual,
                sf3000_vinfo.bits_per_pixel,
                sf3000_vinfo.xoffset, sf3000_vinfo.yoffset,
                sf3000_vinfo.rotate);
    }
    /* Always FBIOPUT + re-assert /dev/dis.  Even when geometry looks correct
     * after hwdisp_deinit, the display controller can still be in driver.so
     * state.  Skipping FBIOPUT in that case → FrogUI shows squished/offset
     * frames.  Cheap to run unconditionally; safe for every entry path. */
    sf3000_vinfo.xres         = 720;
    sf3000_vinfo.yres         = 1280;
    sf3000_vinfo.xres_virtual = 720;
    sf3000_vinfo.yres_virtual = 2560; /* 2 pages; fits in HCGE-leftover smem */
    sf3000_vinfo.xoffset      = 0;
    sf3000_vinfo.yoffset      = 0;
    sf3000_vinfo.bits_per_pixel = 32;
    sf3000_vinfo.rotate       = 0;   /* force panel back to portrait */
    sf3000_vinfo.red    = (struct fb_bitfield){16, 8, 0};
    sf3000_vinfo.green  = (struct fb_bitfield){ 8, 8, 0};
    sf3000_vinfo.blue   = (struct fb_bitfield){ 0, 8, 0};
    sf3000_vinfo.transp = (struct fb_bitfield){24, 8, 0};
    int r2 = ioctl(sf3000_fb_fd, FBIOPUT_VSCREENINFO, &sf3000_vinfo);
    fprintf(stderr, "sf3000_fb_init: FBIOPUT ret=%d\n", r2);
    if (initlog >= 0) dprintf(initlog, "FBIOPUT ret=%d errno=%d\n", r2, errno);
    /* FBIOPUT sets metadata but does NOT pan; driver.so may have left
     * yoffset at a large value (e.g. 3360) which keeps the display scanning
     * out the wrong region (squished/offset frames).  Force pan to (0,0). */
    sf3000_vinfo.xoffset = 0;
    sf3000_vinfo.yoffset = 0;
    int rp = ioctl(sf3000_fb_fd, FBIOPAN_DISPLAY, &sf3000_vinfo);
    DBG("DBG sf3000_fb_init: FBIOPAN_DISPLAY(0,0) ret=%d\n", rp);
    /* Match driver.so's fbdev_init blank sequence: BLANK_NORMAL then
     * UNBLANK. Forces panel to re-latch geometry from FBIOPUT. */
    int rb1 = ioctl(sf3000_fb_fd, FBIOBLANK, 1);
    int rb2 = ioctl(sf3000_fb_fd, FBIOBLANK, 0);
    DBG("DBG sf3000_fb_init: FBIOBLANK(1)=%d (0)=%d\n", rb1, rb2);
    if (initlog >= 0)
        dprintf(initlog, "FBIOBLANK(1)=%d (0)=%d errno=%d\n", rb1, rb2, errno);
    ioctl(sf3000_fb_fd, FBIOGET_VSCREENINFO, &sf3000_vinfo);
    sf3000_dump_fb_state("fb_init/post");
    if (initlog >= 0) {
        dprintf(initlog, "POST-FBIOPUT %ux%u(v%ux%u) bpp=%u off=%u,%u rot=%u\n",
                sf3000_vinfo.xres, sf3000_vinfo.yres,
                sf3000_vinfo.xres_virtual, sf3000_vinfo.yres_virtual,
                sf3000_vinfo.bits_per_pixel,
                sf3000_vinfo.xoffset, sf3000_vinfo.yoffset,
                sf3000_vinfo.rotate);
        fsync(initlog);
        close(initlog);
    }
    ioctl(sf3000_fb_fd, FBIOGET_FSCREENINFO, &sf3000_finfo);

    sf3000_fb_size = sf3000_finfo.smem_len;
    sf3000_fb_mem = mmap(NULL, sf3000_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, sf3000_fb_fd, 0);
    if (sf3000_fb_mem == MAP_FAILED) {
        close(sf3000_fb_fd);
        sf3000_fb_fd = -1;
        return -1;
    }

    fprintf(stderr, "SF3000 fb0: %ux%u (virtual %ux%u) bpp=%u line=%u smem=%u mem=%p\n",
            sf3000_vinfo.xres, sf3000_vinfo.yres,
            sf3000_vinfo.xres_virtual, sf3000_vinfo.yres_virtual,
            sf3000_vinfo.bits_per_pixel,
            sf3000_finfo.line_length, sf3000_finfo.smem_len,
            (void *)sf3000_fb_mem);
    fprintf(stderr, "SF3000 fb0: xoffset=%u yoffset=%u rotate=%u type=%u visual=%u\n",
            sf3000_vinfo.xoffset, sf3000_vinfo.yoffset, sf3000_vinfo.rotate,
            sf3000_finfo.type, sf3000_finfo.visual);
    fprintf(stderr, "SF3000 fb0: pixel r=%u/%u g=%u/%u b=%u/%u a=%u/%u\n",
            sf3000_vinfo.red.offset,   sf3000_vinfo.red.length,
            sf3000_vinfo.green.offset, sf3000_vinfo.green.length,
            sf3000_vinfo.blue.offset,  sf3000_vinfo.blue.length,
            sf3000_vinfo.transp.offset, sf3000_vinfo.transp.length);

    /* Write test pixel, read it back to verify mmap works */
    uint32_t old = sf3000_fb_mem[0];
    sf3000_fb_mem[0] = 0xDEADBEEF;
    uint32_t rb = sf3000_fb_mem[0];
    sf3000_fb_mem[0] = old;
    fprintf(stderr, "SF3000 fb0: write-readback test: wrote 0xDEADBEEF got 0x%08X (%s)\n",
            rb, rb == 0xDEADBEEF ? "OK" : "FAIL - mmap not writable?");

    /* Clear to opaque black once — blit overwrites entirely each frame */
    for (size_t i = 0; i < sf3000_fb_size / 4; i++)
        sf3000_fb_mem[i] = 0xFF000000u;

    sf3000_use_hwdisp = 0;

    /* Consume the warm-boot marker if present (previous picoarch left HCGE
     * active). We no longer NEED it to decide HW — see the unconditional
     * early-init below — but clear it so it can't go stale. */
    unlink("/tmp/picoarch_hcge_was_active");

    /* Init the display driver HERE (fb_init) for ALL devices, not lazily at blit
     * time. fb_init runs BEFORE the audio thread starts (plat_sound_init), so the
     * driver's video init can't race the audio thread's sound_driver_init on the
     * driver's internal error-checking mutex. Doing it at blit time (audio thread
     * already running) raced that mutex → `pthread_mutex_lock: __owner==0` SIGABRT
     * crash-loop (seen on R36SX and the old SF3000 menu experiment). It also gives
     * SF3000 a correct-aspect HW menu from cold (SW transpose squishes 854x480).
     * SF3500 already inited + returned early above. */
    if (!sf3000_use_hwdisp) {
        extern int hwdisp_init(void);
        if (hwdisp_init() == 0) {
            sf3000_use_hwdisp = 1;
            fprintf(stderr, "sf3000_fb_init: hwdisp early-init ok\n");
        } else {
            fprintf(stderr, "sf3000_fb_init: hwdisp early-init failed, SW fallback\n");
        }
    }
    return 0;
}

static int sf3000_last_gh = -1;
static int sf3000_current_page = 0;
static uint16_t *sf3000_rotated_src = NULL;
static int sf3000_rotated_capacity = 0;

struct sf3000_blend_data {
    int16_t gy1, gy2;
    uint8_t w1, w2;
};
static struct sf3000_blend_data sf3000_blend_lut[1024];

static inline uint32_t cvt565(uint16_t c) {
    return 0xFF000000u |
           (((c & 0xF800) << 8) | ((c & 0xE000) << 3)) |
           (((c & 0x07E0) << 5) | ((c & 0x0600) >> 1)) |
           (((c & 0x001F) << 3) | ((c & 0x001C) >> 2));
}

/* Paces one EMULATED frame. Called once per retro_run from the main loop (NOT
 * per present) — cores that frameskip (mame2000) render fewer frames than they
 * emulate, so pacing on present let the skipped frames run unpaced → game ran
 * too fast (e.g. 3x). Pace per emulated frame instead. */
void sf3000_frame_limit(void) {
    extern int g_ff_level;            /* 0=off,1=2x,2=3x,3=uncapped */
    extern double frame_rate;         /* core's native fps (retro_get_system_av_info) */
    if (g_ff_level == 3) return;      /* uncapped: no pacing, run flat out */
    /* Pace to the CORE's frame rate, not a hardcoded 60 — cores whose native
     * rate isn't 60 (ecwolf 35fps, some arcade games) otherwise run too fast
     * (and their frame-time callback / audio desync from the mismatch). Falls
     * back to 60fps if the core hasn't reported a rate yet. */
    long long base = (frame_rate > 1.0) ? (long long)(1000000.0 / frame_rate) : 16666;
    long long target = base / (g_ff_level + 1);   /* pace emulation faster on FF */
    static struct timeval last_tv = {0, 0};
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (last_tv.tv_sec != 0) {
        long long diff_us = (tv.tv_sec - last_tv.tv_sec) * 1000000LL + (tv.tv_usec - last_tv.tv_usec);
        if (diff_us > 0 && diff_us < target) {
            long long sleep_us = target - diff_us;
            if (sleep_us > 2000)
                usleep(sleep_us - 2000);
            while (1) {
                gettimeofday(&tv, NULL);
                diff_us = (tv.tv_sec - last_tv.tv_sec) * 1000000LL + (tv.tv_usec - last_tv.tv_usec);
                if (diff_us >= target) break;
            }
        }
    }
    last_tv = tv;
}

/* Runtime device detection — one SD image, N devices, profile chosen at boot.
 *
 * Primary source: the boot detector (cubegm/tf_detect.sh) writes
 * /tmp/tfdevice.env (TF_DEVICE + panel geometry) before any frontend launches.
 * Fallback: if that file is absent (dev / standalone run), scan the live
 * device-tree at /proc/device-tree using the same rules the script uses.
 *
 * Per-device facts (all derived from the stock dtb's panel nodes):
 *   R36SX  — panel 640x480 landscape, 4:3,  driver_r36sx.so,  present = fb-write
 *            (panel chip r63311; its disp_frame hangs → direct fb0 write)
 *   SF3000 — panel 480x854 portrait → 854x480 16:9, driver_sf3000.so,
 *            present = disp_frame (90° CW rotate). DT: /hcrtos/lcd, no /panel.
 *   SF3500 — panel geometry identical to SF3000 (same timings); only the
 *            driver.so binary differs. driver_sf3500.so, present = disp_frame.
 *            DT: /panel node present (lcd-type=1), no r63311.
 *
 * Detection rule (file or DT both resolve to one of these):
 *   r63311 node present            → R36SX
 *   else /panel node present       → SF3500
 *   else (/hcrtos/lcd, no /panel)  → SF3000
 *
 * "present = fb-write" (R36SX only) is the single behavioural split downstream;
 * SF3500 follows SF3000 in every code path, differing only by driver file. */
extern void dbg_log(const char *fmt, ...);
/* Device-id enum lives with the speaker-gate board table (first consumer)
 * and the runtime detection below - one definition, all users. */
static int g_dev_id = -1;       /* -1 = undetected */
static int g_dev_r36sx = -1;    /* derived: 1 if R36SX (the fb-write device) */
static int g_dev_w = 854, g_dev_h = 480, g_dev_scale = 150;

/* Recursively scan /proc/device-tree for the R36SX panel node (r63311), without
 * forking — popen/fork inside the emulator process can disturb later dynarec
 * memory mappings (gpsp). Bounded depth. Sets *found if seen. */
static void dt_scan_r63311(const char *dir, int depth, int *found) {
    if (*found || depth > 5) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[512];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (strstr(e->d_name, "r63311")) { *found = 1; break; }
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        /* recurse into subdirs only */
        DIR *sub = opendir(path);
        if (sub) { closedir(sub); dt_scan_r63311(path, depth + 1, found); if (*found) break; }
    }
    closedir(d);
}

/* Map a device name (from env file or DT scan) to the id enum. Unknown → -1. */
static int tf_id_from_name(const char *n) {
    if (!n) return -1;
    if (!strcasecmp(n, "R36SX"))  return TF_DEV_R36SX;
    if (!strcasecmp(n, "SF3500")) return TF_DEV_SF3500;
    if (!strcasecmp(n, "SF3000")) return TF_DEV_SF3000;
    if (!strcasecmp(n, "GB350"))  return TF_DEV_GB350;
    return -1;
}

/* Read TF_DEVICE / TF_PANEL_W / TF_PANEL_H / TF_UI_SCALE from the boot env file.
 * Returns the device id (or -1 if the file is missing / has no usable TF_DEVICE).
 * Geometry lines are optional overrides applied to the w/h/scale outputs. */
static int tf_read_env_file(int *w, int *h, int *scale) {
    FILE *f = fopen("/tmp/tfdevice.env", "r");
    if (!f) return -1;
    int id = -1;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        char *nl = strpbrk(val, "\r\n"); if (nl) *nl = 0;
        if      (!strcmp(key, "TF_DEVICE"))   id = tf_id_from_name(val);
        else if (!strcmp(key, "TF_PANEL_W"))  { int v = atoi(val); if (v > 0) *w = v; }
        else if (!strcmp(key, "TF_PANEL_H"))  { int v = atoi(val); if (v > 0) *h = v; }
        else if (!strcmp(key, "TF_UI_SCALE")) { int v = atoi(val); if (v > 0) *scale = v; }
    }
    fclose(f);
    return id;
}

void sf3000_detect_device(void) {
    if (g_dev_id != -1) return;

    int w = 854, h = 480, scale = 150;        /* defaults (SF3000/SF3500) */
    int id = tf_read_env_file(&w, &h, &scale); /* primary: boot env file */
    if (id < 0) id = tf_id_from_name(getenv("TF_DEVICE")); /* inherited env */
    if (id < 0) {                              /* fallback: scan live DT */
        int r63311 = 0;
        dt_scan_r63311("/proc/device-tree", 0, &r63311);
        if (r63311)
            id = TF_DEV_R36SX;
        else if (access("/proc/device-tree/panel", F_OK) == 0) {
            /* /panel devices are 854x480 SF3500-class — EXCEPT GB350, which is
             * 640x480 and is the only /panel device lacking BOTH hcrtos/
             * multiple_init (present on SF3000HD) and hcrtos/uart@1 (present on
             * SF3500/SF3100). */
            if (access("/proc/device-tree/hcrtos/multiple_init", F_OK) == 0 ||
                access("/proc/device-tree/hcrtos/uart@1", F_OK) == 0)
                id = TF_DEV_SF3500;
            else
                id = TF_DEV_GB350;
        } else
            id = TF_DEV_SF3000;
    }

    /* Panel geometry by device (env-file overrides above already applied to
     * w/h when present; otherwise set the known per-device defaults). */
    if (id == TF_DEV_R36SX || id == TF_DEV_GB350) { if (w == 854) w = 640; }  /* landscape 640x480, 4:3 */
    g_dev_id    = id;
    g_dev_r36sx = (id == TF_DEV_R36SX);
    g_dev_w = w; g_dev_h = h; g_dev_scale = scale;

    char s[16];
    snprintf(s, sizeof s, "%d", g_dev_w);     setenv("TF_PANEL_W", s, 1);
    snprintf(s, sizeof s, "%d", g_dev_h);     setenv("TF_PANEL_H", s, 1);
    snprintf(s, sizeof s, "%d", g_dev_scale); setenv("TF_UI_SCALE", s, 1);
    dbg_log("DBG device: %s panel=%dx%d ui_scale=%d\n",
            id == TF_DEV_R36SX ? "R36SX" : id == TF_DEV_SF3500 ? "SF3500" :
            id == TF_DEV_GB350 ? "GB350" : "SF3000",
            g_dev_w, g_dev_h, g_dev_scale);
}

int sf3000_panel_w(void)  { sf3000_detect_device(); return g_dev_w; }
int sf3000_panel_h(void)  { sf3000_detect_device(); return g_dev_h; }
/* Raw TF_DEV_* id for the running console (used by the speaker-gate board
 * table to fail closed on unvalidated devices). */
int sf3000_device_id(void) { sf3000_detect_device(); return g_dev_id; }
int sf3000_is_r36sx(void) { sf3000_detect_device(); return g_dev_r36sx > 0; }
int sf3000_is_sf3500(void) { sf3000_detect_device(); return g_dev_id == TF_DEV_SF3500; }
int sf3000_is_gb350(void) { sf3000_detect_device(); return g_dev_id == TF_DEV_GB350; }
/* 4:3 panels (R36SX, GB350 = 640x480) vs 16:9 (SF3000/SF3500 = 854x480). */
int sf3000_aspect_is_43(void) { sf3000_detect_device(); return g_dev_r36sx > 0 || g_dev_id == TF_DEV_GB350; }

/* Device-correct driver.so path. NOTE: SF3500's STOCK driver.so is ENCRYPTED
 * (firmware decrypts it to /tmp/cubegm/driver.so at boot). We grab that plaintext
 * at runtime (zhijack) and ship it AS driver_sf3500.so — a real ELF picoarch can
 * dlopen for HW video + audio. */
const char *sf3000_driver_path(void) {
    sf3000_detect_device();
    switch (g_dev_id) {
    case TF_DEV_R36SX:  return "/mnt/sdcard/cubegm/driver_r36sx.so";
    case TF_DEV_SF3500: return "/mnt/sdcard/cubegm/driver_sf3500.so"; /* decrypted, real driver */
    case TF_DEV_GB350:  return "/mnt/sdcard/cubegm/driver_gb350.so";  /* plain ELF, 640x480 */
    default:            return "/mnt/sdcard/cubegm/driver_sf3000.so";
    }
}

#define PANEL_W (sf3000_panel_w())
#define PANEL_H (sf3000_panel_h())
#define PANEL_ASPECT_NUM (sf3000_aspect_is_43() ? 4 : 16)
#define PANEL_ASPECT_DEN (sf3000_aspect_is_43() ? 3 : 9)

/* Unified "Aspect ratio" control (menu). One list that picks BOTH the scale mode
 * and the target ratio, so there's no separate Screen-size knob to conflict with:
 *   Integer   -> exact pixel multiples (no reshape)
 *   Native    -> whatever the core reports
 *   4:3 .. 16:10 -> forced ratio (reshapes to it)
 *   Fill      -> stretch to the whole panel
 * num==0 means "no forced ratio" (Native/Integer/Fill). ss drives the existing
 * scale_size machinery. */
struct aspect_def { const char *name; int ss; int num, den; };
static const struct aspect_def aspect_defs[] = {
    { "Integer", SCALE_SIZE_NONE,   0,  0 },
    { "Native",  SCALE_SIZE_ASPECT, 0,  0 },
    { "4:3",     SCALE_SIZE_ASPECT, 4,  3 },
    { "16:9",    SCALE_SIZE_ASPECT, 16, 9 },
    { "3:2",     SCALE_SIZE_ASPECT, 3,  2 },
    { "5:4",     SCALE_SIZE_ASPECT, 5,  4 },
    { "8:7",     SCALE_SIZE_ASPECT, 8,  7 },
    { "16:10",   SCALE_SIZE_ASPECT, 16, 10 },
    { "Fill",    SCALE_SIZE_FULL,   0,  0 },
};
#define ASPECT_N ((int)(sizeof(aspect_defs)/sizeof(aspect_defs[0])))
const char *const *sf3000_aspect_names(void) {
    static const char *names[ASPECT_N + 1];
    if (!names[0]) { for (int i = 0; i < ASPECT_N; i++) names[i] = aspect_defs[i].name; names[ASPECT_N] = NULL; }
    return names;
}
/* Keep scale_size in sync with the chosen aspect mode (the merged control drives
 * it). Cheap; called each blit so a menu change takes effect next frame. */
static void sf3000_apply_aspect(void) {
    if (aspect_ratio_mode < 0 || aspect_ratio_mode >= ASPECT_N) aspect_ratio_mode = 1;
    scale_size = (enum scale_size)aspect_defs[aspect_ratio_mode].ss;
}
static double sf3000_content_aspect(void) {
    int m = (aspect_ratio_mode >= 0 && aspect_ratio_mode < ASPECT_N) ? aspect_ratio_mode : 1;
    const struct aspect_def *d = &aspect_defs[m];
    if (d->num > 0) return (double)d->num / d->den;
    return (aspect_ratio > 0.1) ? aspect_ratio : 4.0 / 3.0;   /* Native */
}
static int sf3000_aspect_forced(void) {
    int m = (aspect_ratio_mode >= 0 && aspect_ratio_mode < ASPECT_N) ? aspect_ratio_mode : 1;
    return aspect_defs[m].num > 0;
}

/* Preserve a forced ratio by reshaping only the small core frame; the stock
 * driver still performs the expensive enlargement to the panel in hardware. */
static void sf3000_hw_present_frame(const void *src, int w, int h, int pitch,
                                    int game_frame) {
    /* SF3000/SF3500 use the historical full-panel integer canvas because
     * their driver does not reliably honor viewport-only integer scaling.
     * R36SX keeps its separate direct framebuffer path. */
    /* The 854x480 integer canvas is specific to SF3000/SF3500.  GB350 is a
     * 640x480 panel and must remain on its normal R36-style presentation path. */
    if (game_frame && scale_size == SCALE_SIZE_NONE &&
        !sf3000_is_r36sx() && !sf3000_is_gb350()) {
        hwdisp_present_integer(src, w, h, pitch);
        return;
    }

    if (!(sf3000_is_r36sx() && hwdisp_present_direct(src, w, h, pitch)))
        hwdisp_present(src, w, h, pitch);
}
/* Screenshot: dump the current RGB565 frame to a top-down 24bpp BMP. */
static void sf3000_screenshot(const void *src, int w, int h, int pitch) {
    if (!src || w <= 0 || h <= 0) return;
    system("mkdir -p /mnt/sdcard/screenshots 2>/dev/null");
    static int n = 0;
    char path[96];
    snprintf(path, sizeof path, "/mnt/sdcard/screenshots/shot_%03d.bmp", n++);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int rowb = (w * 3 + 3) & ~3;           /* BMP rows padded to 4 bytes */
    int isz = rowb * h, fsz = 54 + isz, neg = -h;  /* negative h = top-down */
    unsigned char hd[54] = {0};
    hd[0]='B'; hd[1]='M';
    hd[2]=fsz; hd[3]=fsz>>8; hd[4]=fsz>>16; hd[5]=fsz>>24; hd[10]=54;
    hd[14]=40;
    hd[18]=w; hd[19]=w>>8; hd[20]=w>>16; hd[21]=w>>24;
    hd[22]=neg; hd[23]=neg>>8; hd[24]=neg>>16; hd[25]=neg>>24;
    hd[26]=1; hd[28]=24;
    fwrite(hd, 1, 54, f);
    int sp = pitch / 2;
    const uint16_t *s = (const uint16_t *)src;
    unsigned char *line = (unsigned char *)calloc(1, rowb);
    if (line) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint16_t p = s[(size_t)y * sp + x];
                int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                line[x*3]   = (b << 3) | (b >> 2);   /* B */
                line[x*3+1] = (g << 2) | (g >> 4);   /* G */
                line[x*3+2] = (r << 3) | (r >> 2);   /* R */
            }
            fwrite(line, 1, rowb, f);
        }
        free(line);
    }
    fclose(f);
    sync();
    fprintf(stderr, "screenshot saved: %s\n", path);
}

void sf3000_fb_blit(const void *src, int width, int height, int pitch) {
    sf3000_apply_aspect();   /* merged Aspect-ratio control drives scale_size */
    /* Oversized frames use a panel canvas. Normal game frames go directly
     * through the HCGE driver; SF3000's small-frame surface is patched to the
     * native 854x480 geometry during hwdisp_init, so no full-panel software
     * scaling is performed here. */
    /* STICKY: once one frame composes, compose every later frame too. Cores
     * that zoom (G&W Multi Screen alternates full view 606x748 <-> zoomed
     * 343x193) otherwise flap the frame size, and each size change makes the
     * driver re-init its geometry (hcge_driver_init churn) — which wedges it. */
    static int compose_sticky = 0;
    if ((compose_sticky || width > 640 || height > 480) &&
        !(width == PANEL_W && height == PANEL_H)) {
        {
            extern void dbg_log(const char *fmt, ...);
            static int c_last_w = -1, c_last_h = -1;
            if (width != c_last_w || height != c_last_h) {
                dbg_log("DBG compose: core frame %dx%d pitch=%d -> %dx%d canvas\n",
                        width, height, pitch, PANEL_W, PANEL_H);
                c_last_w = width; c_last_h = height;
            }
        }
        compose_sticky = 1;
        static uint16_t *fit_buf = NULL;
        static int fit_cap = 0;
        int dw = PANEL_W, dh = PANEL_H;
        if (scale_size != SCALE_SIZE_FULL) {
            double target_aspect = sf3000_content_aspect();
            dw = (int)(PANEL_H * target_aspect + 0.5);
            if (dw > PANEL_W) {
                dw = PANEL_W;
                dh = (int)(PANEL_W / target_aspect + 0.5);
            }
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        if (PANEL_W * PANEL_H > fit_cap) {
            free(fit_buf);
            fit_cap = PANEL_W * PANEL_H;
            fit_buf = (uint16_t *)malloc((size_t)fit_cap * sizeof(uint16_t));
        }
        if (fit_buf) {
            const uint8_t *s8 = (const uint8_t *)src;
            int ox = (PANEL_W - dw) / 2, oy = (PANEL_H - dh) / 2;
            uint32_t xstep = ((uint32_t)width << 16) / dw;
            uint32_t ystep = ((uint32_t)height << 16) / dh;
            uint32_t yacc = 0;
            memset(fit_buf, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
            for (int y = 0; y < dh; y++, yacc += ystep) {
                const uint16_t *srow =
                    (const uint16_t *)(s8 + (size_t)(yacc >> 16) * pitch);
                uint16_t *drow = fit_buf + (size_t)(oy + y) * PANEL_W + ox;
                uint32_t xacc = 0;
                for (int x = 0; x < dw; x++, xacc += xstep)
                    drow[x] = srow[xacc >> 16];
            }
            src = fit_buf;
            width = PANEL_W;
            height = PANEL_H;
            pitch = PANEL_W * 2;
        }
    }
    /* Screenshot hotkey: SELECT (bit0) + L1 (bit10). Capture whatever's on screen
     * (game/FrogUI/menu frame), rising-edge so one shot per press. L1 picked
     * because R2/R1/L2/START/B/Y are already taken by the OnionOS hotkeys. */
    {
        extern volatile uint32_t sf3000_keys_filtered;
        static int prev_combo = 0;
        uint32_t combo = (1u << 0) | (1u << 10);
        int now = (sf3000_keys_filtered & combo) == combo;
        if (now && !prev_combo) sf3000_screenshot(src, width, height, pitch);
        prev_combo = now;
    }
    {
        extern void dbg_log(const char *fmt, ...);
        static int s_blit = 0;
        static int s_last_w = -1, s_last_h = -1;
        /* re-log on every frame-size change (zoom, mode switch), not just the
         * first 8 frames — size transitions are where display bugs live */
        if (width != s_last_w || height != s_last_h) {
            if (s_last_w != -1)
                dbg_log("DBG blit SIZE CHANGE: %dx%d -> %dx%d\n",
                        s_last_w, s_last_h, width, height);
            s_last_w = width; s_last_h = height;
            s_blit = 0;
        }
        if (s_blit < 24) {
            s_blit++;
            dbg_log("DBG blit#%d: src=%p screen=%p w=%d h=%d pitch=%d scale_filter=%d scale_size=%d use_hw=%d PANEL=%dx%d\n",
                    s_blit, src, (void*)screen->pixels, width, height, pitch,
                    scale_filter, scale_size, sf3000_use_hwdisp, PANEL_W, PANEL_H);
            fprintf(stderr, "TFDBG blit#%d w=%d h=%d pitch=%d filter=%d scale=%d use_hw=%d\n",
                    s_blit, width, height, pitch, scale_filter, scale_size, sf3000_use_hwdisp);
        }
    }
    /* R36SX: pass the user's scale-size choice (integer/aspect/full) to the HW
     * present so disp_frame fills / aspect-fits / integer-scales accordingly. */
    hwdisp_set_panel_scale((int)scale_size);
    /* WARM-BOOT: hwdisp pre-init'd by sf3000_fb_init via marker file.
     * For FrogUI panel-size frames, force HW bilinear pass-through — only path
     * proven safe with panel-native input.  Cold-boot FrogUI takes SW path
     * (no marker → use_hwdisp=0). */
    /* R36SX ONLY: panel-size core frames (FrogUI) → direct fb0 write (controller
     * scales, panel is landscape-native so no rotation needed). SF3000 composed
     * game frames and FrogUI use the driver's verified panel-size path; its
     * 270-degree rotation is handled inside driver.so. */
    if (sf3000_is_r36sx() && src != screen->pixels &&
        width == PANEL_W && height == PANEL_H) {
        if (!sf3000_use_hwdisp && hwdisp_init() == 0) {
            sf3000_use_hwdisp = 1;
            fprintf(stderr, "sf3000_fb_blit: HW path active (panel-size)\n");
        }
        if (sf3000_use_hwdisp && hwdisp_present_direct(src, width, height, pitch))
            return;
    }
    /* Lazy-init HW: R36SX always (disp_frame hangs → fb-write for everything);
     * SF3000 only on bilinear frames (nearest games take the SW path). */
    /* SF3500: always present via the real driver's disp_frame (decrypted driver
     * handles panel-size frames cleanly — proven by rkgame's own FrogUI render).
     * The SW transpose path is SF3000-geometry-tuned → squish + page-flip churn
     * on SF3500, so route everything (incl. nearest/menu frames) through HW. */
    /* SF3000 panel-size frames are now safe: FrogUI already exercises this path
     * on every boot, and composed game frames use the same source geometry. */
    sf3000_detect_device();
    if (!sf3000_use_hwdisp &&
        (sf3000_is_r36sx() || sf3000_is_sf3500() ||
         scale_filter == SCALE_FILTER_BILINEAR)) {
        if (hwdisp_init() == 0) {
            sf3000_use_hwdisp = 1;
            fprintf(stderr, "sf3000_fb_blit: HW path active\n");
        }
    }


if (sf3000_use_hwdisp) {
        int game_frame = (src != screen->pixels);
        /* HW edge-sharpen: skip only for frames that actually take the pixel-exact
         * SW-nearest path (small game frames under Nearest). Panel-size frames
         * (FrogUI menu, full-panel game) always go through the driver's HW bilinear
         * scale regardless of filter, so they still need the enhance — gating on the
         * global scale_filter alone left the menu blurry after a nearest game.
         * The width/height test mirrors hwdisp's own `game` flag. Applied on change. */
        int frame_sw_nearest = (scale_filter == SCALE_FILTER_NEAREST) &&
                               !(width == PANEL_W && height == PANEL_H);
        hwdisp_set_sharpen(frame_sw_nearest ? 0 : sf3000_sharpness);
        /* Fast-forward: frame_limit paces emulation at (level+1)*60fps; here we
         * present only 1 of (level+1) frames so the display stays 60fps and the
         * game runs (level+1)x. frame_limit runs first (below) → paces every
         * frame; this only skips the present. Game frames only (not FrogUI). */
        extern int g_ff_level;
        int ff_skip = 0;
        if (g_ff_level && src != screen->pixels) {
            static unsigned ff_ctr = 0;
            /* 2x/3x: present 1 of (level+1). Uncapped(3): no pacing, so present
             * every 2nd emulated frame (frameskip 1) to cut present cost. */
            int div = (g_ff_level == 3) ? 2 : (g_ff_level + 1);
            if ((++ff_ctr) % div) ff_skip = 1;
        }
        int aspect = !game_frame || (scale_size != SCALE_SIZE_FULL);
        /* Native portrait/vector frames must retain the core's envelope.
         * Padding VecX 330x410 to the SF panel's 16:9 envelope produces
         * 728x410, beyond disp_frame's 640px source-width limit: the driver
         * rejects every frame and leaves a black screen. Forced modes have
         * already reshaped the small frame; menus still use the panel aspect. */
        if (game_frame && scale_size == SCALE_SIZE_ASPECT &&
            sf3000_aspect_forced()) {
            /* Forced ratios must reach the driver's aspect-pad stage.  The
             * old code always passed the physical panel ratio here (16:9 on
             * SF3000), so a requested 4:3/5:4/etc. was silently padded as the
             * panel ratio and appeared to snap back toward the core's small
             * native envelope (notably 8:7 NES output). */
            int m = (aspect_ratio_mode >= 0 && aspect_ratio_mode < ASPECT_N) ? aspect_ratio_mode : 1;
            hwdisp_set_target_aspect(aspect_defs[m].num, aspect_defs[m].den);
        } else if (game_frame && scale_size == SCALE_SIZE_ASPECT &&
                   !sf3000_aspect_forced()) {
            double ar = (aspect_ratio > 0.1) ? aspect_ratio : (4.0 / 3.0);
            int native_num = (int)(ar * 1000.0 + 0.5);
            if (native_num < 1) native_num = 1;
            hwdisp_set_target_aspect(native_num, 1000);
        } else {
            hwdisp_set_target_aspect(aspect ? PANEL_ASPECT_NUM : 0,
                                     aspect ? PANEL_ASPECT_DEN : 0);
        }
        {
            extern void dbg_log(const char *fmt, ...);
            static int last_log_w = -1, last_log_h = -1, last_log_mode = -1;
            static int last_log_aspect = -1, last_log_filter = -1;
            if (width != last_log_w || height != last_log_h ||
                scale_size != last_log_mode || aspect_ratio_mode != last_log_aspect ||
                scale_filter != last_log_filter) {
                dbg_log("DBG HW geometry: src=%dx%d game=%d scale=%d aspect_mode=%d forced=%d core_ar=%.4f requested_filter=%d effective_filter=%s\n",
                        width, height, game_frame, (int)scale_size,
                        aspect_ratio_mode, sf3000_aspect_forced(), aspect_ratio,
                        scale_filter, sf3000_is_r36sx() ? "selected" : "hw-bilinear");
                last_log_w = width; last_log_h = height; last_log_mode = scale_size;
                last_log_aspect = aspect_ratio_mode; last_log_filter = scale_filter;
            }
        }
        /* Filter routing. The SW Nearest/Sharp paths are R36SX-only (present_direct
         * + panel_build); SF-class presents through the driver's disp_frame HW
         * scaler, where the SW-nearest upscale mis-sizes the frame - so SF-class
         * stays on HW bilinear regardless of the setting.
         *   R36SX game  → the chosen filter.
         *   R36SX menu  → Nearest (320x240 → 640x480 is exact 2x = crisp UI).
         *   SF-class    → always Bilinear (HW). */
        int fmode;
        if (!sf3000_is_r36sx())            fmode = SCALE_FILTER_BILINEAR;
        else if (src != screen->pixels)    fmode = scale_filter;
        else                               fmode = SCALE_FILTER_NEAREST;
        hwdisp_set_filter(fmode);

        /* FPS / overlay text: render into a copy of src (RGB565), since src
         * itself is owned by the core and may be read-only. */
        if (msg[0]) {
            static uint16_t *compose_buf = NULL;
            static int compose_cap = 0;
            int need = width * height;
            if (need > compose_cap) {
                free(compose_buf);
                compose_cap = need + 4096;
                compose_buf = (uint16_t*)malloc(compose_cap * sizeof(uint16_t));
            }
            if (compose_buf) {
                /* Copy src into compose_buf row-by-row (pitch may be wider). */
                int sp = pitch / 2;
                const uint16_t *s = (const uint16_t *)src;
                for (int y = 0; y < height; y++)
                    memcpy(compose_buf + (size_t)y * width, s + (size_t)y * sp, (size_t)width * 2);

                /* Render msg at top-left using fontdata8x8. The buffer is the
                 * game's native res and gets upscaled to the panel, so a fixed
                 * scale looks huge on low-res cores. Scale with buffer height so
                 * the on-panel size stays ~constant (≈ the menu font). */
                int len = 0; while (msg[len]) len++;
                /* The HUD msg is space-padded to a fixed width (fps left / cpu
                 * right), so size the bar to the last non-space glyph — otherwise
                 * it stretches across the whole screen. */
                int vis = len; while (vis > 0 && msg[vis-1] == ' ') vis--;
                int ts = height / 120; if (ts < 1) ts = 1;
                int tx = 4, ty = 4;
                /* Background bar */
                int bw = vis * 8 * ts + 4;
                int bh = 8 * ts + 4;
                for (int by = 0; by < bh && (ty + by) < height; by++) {
                    uint16_t *r = compose_buf + (size_t)(ty + by) * width + tx;
                    int span = bw; if (tx + span > width) span = width - tx;
                    for (int bx = 0; bx < span; bx++) r[bx] = 0;
                }
                /* Glyphs (white = 0xFFFF) */
                for (int i = 0; i < len; i++) {
                    unsigned char c = (unsigned char)msg[i];
                    for (int row = 0; row < 8; row++) {
                        unsigned char fd = fontdata8x8[c * 8 + row];
                        if (!fd) continue;
                        for (int sr = 0; sr < ts; sr++) {
                            int py = ty + 2 + row * ts + sr;
                            if (py >= height) break;
                            uint16_t *r = compose_buf + (size_t)py * width;
                            for (int col = 0; col < 8; col++) {
                                if (!(fd & (0x80 >> col))) continue;
                                int px = tx + 2 + i * 8 * ts + col * ts;
                                for (int sc = 0; sc < ts && (px + sc) < width; sc++)
                                    r[px + sc] = 0xFFFF;
                            }
                        }
                    }
                }
                if (ff_skip) return;    /* FF: drop this present to keep display 60fps */
                /* R36SX: disp_frame hangs → direct fb0 write. SF3000: disp_frame. */
                sf3000_hw_present_frame(compose_buf, width, height, width * 2, 1);
                return;
            }
        }
        if (ff_skip) return;    /* FF: drop this present to keep display 60fps */
        /* SF3500: the picoarch pause menu renders to the SDL surface (320x240) →
         * disp_frame tiles that odd size. Nearest-scale ONLY the menu surface up
         * to PANEL; game/FrogUI core frames keep their own HW scaling + aspect
         * (scaling EVERY frame here was far too slow and forced fullscreen). */
        if (sf3000_is_sf3500() && src == screen->pixels) {
            /* disp_frame DMA-reads src asynchronously; handing it a single
             * static buffer races the next redraw's scaling (glyphs/pixels
             * vanish while scrolling the pause menu). Ping-pong buffers so
             * the engine always scans a stable copy. */
            static uint16_t *mb[2];
            static int mbi;
            if (!mb[0]) { mb[0] = (uint16_t*)malloc(PANEL_W * PANEL_H * 2); mb[1] = (uint16_t*)malloc(PANEL_W * PANEL_H * 2); }
            uint16_t *dst = mb[mbi]; mbi ^= 1;
            if (dst) {
                const uint16_t *s = (const uint16_t*)src; int sp = pitch/2;
                for (int y = 0; y < PANEL_H; y++) {
                    const uint16_t *srow = s + (size_t)(y * height / PANEL_H) * sp;
                    uint16_t *drow = dst + (size_t)y * PANEL_W;
                    for (int x = 0; x < PANEL_W; x++) drow[x] = srow[x * width / PANEL_W];
                }
                src = dst; width = PANEL_W; height = PANEL_H; pitch = PANEL_W * 2;
            }
        }
        sf3000_hw_present_frame(src, width, height, pitch, game_frame);
        return;
    }
    /* Re-assert display controller → fb0 page.
       Virtual fb is 5120 lines (4 pages). This ioctl keeps display pointed at our page. */
    {
        int dis = open("/dev/dis", O_RDWR);
        if (dis >= 0) {
            struct { int a, b, c; } buf = {1, 0, 0};
            ioctl(dis, 0xc00c0e0c, &buf);
            close(dis);
        }
    }

    if (!sf3000_fb_mem) return;

    int dst_stride = (int)(sf3000_finfo.line_length / 4);
    const int FB_X_VIS = 180;
    const int FB_Y_TOT = 1014;   /* visible fb_y range: fb_y 0..1013 */
    const int PHYS_W   = 854;
    const int PHYS_H   = 480;
    const int PAGE_Y_SIZE = 1280;

    const int gw = width, gh = height;
    const uint16_t *s = (const uint16_t *)src;

    if (gw * gh > sf3000_rotated_capacity) {
        if (sf3000_rotated_src) free(sf3000_rotated_src);
        sf3000_rotated_capacity = gw * gh + 8192;
        sf3000_rotated_src = malloc(sf3000_rotated_capacity * sizeof(uint16_t));
    }

    /* Fast, pure-data transpose pass. Great for CPU cache. */
    for (int gy = 0; gy < gh; gy++) {
        const uint16_t *src_row = (const uint16_t *)((const char *)s + gy * pitch);
        for (int gx = 0; gx < gw; gx++) {
            sf3000_rotated_src[gx * gh + gy] = src_row[gx];
        }
    }

    int current_scale = scale_size;
    if (src == screen->pixels) {
        current_scale = SCALE_SIZE_ASPECT; /* Force menu to not stretch */
    }

    int disp_w = PHYS_W;
    int disp_h = PHYS_H;
    
    if (current_scale == SCALE_SIZE_ASPECT) {
        /* Use the unified frontend selection here too.  The SF3000 software
         * transpose path used to read only the core-reported aspect_ratio,
         * which silently turned a requested 3:2/4:3 mode back into the NES
         * core's native envelope. */
        double ar = sf3000_content_aspect();
        if (ar <= 0.1) ar = (double)gw / gh;
        disp_w = (int)(PHYS_H * ar + 0.5);
    } else if (current_scale == SCALE_SIZE_NONE) {
        /* Integer scaling */
        int scale_x = PHYS_W / gw;
        int scale_y = PHYS_H / gh;
        int scale = (scale_x < scale_y) ? scale_x : scale_y;
        if (scale < 1) scale = 1;
        disp_w = gw * scale;
        disp_h = gh * scale;
    }
    
    if (disp_w > PHYS_W) disp_w = PHYS_W;
    if (disp_h > PHYS_H) disp_h = PHYS_H;
    if (current_scale == SCALE_SIZE_FULL) {
        disp_w = PHYS_W;
        disp_h = PHYS_H;
    }

    static int sf3000_last_disp_h = -1;
    if (gh != sf3000_last_gh || disp_h != sf3000_last_disp_h) {
        int fb_x_len = disp_h * FB_X_VIS / PHYS_H;
        int fb_x_off = (FB_X_VIS - fb_x_len) / 2;

        for (int fb_x = 0; fb_x < FB_X_VIS; fb_x++) {
            if (fb_x < fb_x_off || fb_x >= fb_x_off + fb_x_len) {
                sf3000_blend_lut[fb_x].gy1 = -1; // Black border
                sf3000_blend_lut[fb_x].w2 = 0;
            } else {
                int active_x = fb_x - fb_x_off;
                int gy_fixed = ((fb_x_len - 1 - active_x) * gh * 256) / fb_x_len;
                int gy = gy_fixed >> 8;
                int frac = gy_fixed & 0xFF;
                
                sf3000_blend_lut[fb_x].gy1 = gy;
                sf3000_blend_lut[fb_x].gy2 = (gy + 1 < gh) ? (gy + 1) : gy;
                sf3000_blend_lut[fb_x].w2 = frac >> 1; // 0..127
                sf3000_blend_lut[fb_x].w1 = 128 - (frac >> 1);
            }
        }
        sf3000_last_gh = gh;
        sf3000_last_disp_h = disp_h;
    }

    /* Convert physical width → fb_y units, then center */
    int fb_y_len = disp_w * FB_Y_TOT / PHYS_W;
    int fb_y_off = (FB_Y_TOT - fb_y_len) / 2;

    static uint32_t row_cache[180];
    static int last_fb_y_off = -1, last_fb_y_len = -1;

    /* Cycle between page 0 and page 1 for double buffering */
    sf3000_current_page = (sf3000_current_page + 1) % 2;
    int page_y_offset = sf3000_current_page * PAGE_Y_SIZE;

    if (fb_y_off != last_fb_y_off || fb_y_len != last_fb_y_len) {
        for (int p = 0; p < 2; p++) {
            for (int fy = 0; fy < PAGE_Y_SIZE; fy++) {
                for (int fx = 0; fx < FB_X_VIS; fx++) {
                    sf3000_fb_mem[(p * PAGE_Y_SIZE + fy) * dst_stride + fx] = 0xFF000000u;
                }
            }
        }
        last_fb_y_off = fb_y_off;
        last_fb_y_len = fb_y_len;
    }

    int fb_y = fb_y_off;
    while (fb_y < fb_y_off + fb_y_len) {
        int gx = (fb_y - fb_y_off) * gw / fb_y_len;
        
        /* Count how many lines share this exact gx (nearest neighbor vertical scaling) */
        int count = 1;
        while (fb_y + count < fb_y_off + fb_y_len && ((fb_y + count - fb_y_off) * gw / fb_y_len) == gx) {
            count++;
        }
        
        /* Compute the blended 32-bit row ONLY ONCE for these `count` lines */
        uint16_t *src_col = sf3000_rotated_src + gx * gh;
        for (int fb_x = 0; fb_x < FB_X_VIS; fb_x++) {
            int gy1 = sf3000_blend_lut[fb_x].gy1;
            if (gy1 < 0) {
                row_cache[fb_x] = 0xFF000000u;
                continue;
            }
            int gy2 = sf3000_blend_lut[fb_x].gy2;
            uint32_t w1 = sf3000_blend_lut[fb_x].w1;
            uint32_t w2 = sf3000_blend_lut[fb_x].w2;

            uint16_t p1 = src_col[gy1];

            if (w2 == 0 || gy1 == gy2) {
                row_cache[fb_x] = cvt565(p1);
            } else {
                uint16_t p2 = src_col[gy2];
                if (p1 == p2) {
                    row_cache[fb_x] = cvt565(p1);
                } else {
                    uint32_t c1 = cvt565(p1);
                    uint32_t c2 = cvt565(p2);
                    uint32_t rb = (((c1 & 0xFF00FF) * w1 + (c2 & 0xFF00FF) * w2) >> 7) & 0xFF00FF;
                    uint32_t g  = (((c1 & 0x00FF00) * w1 + (c2 & 0x00FF00) * w2) >> 7) & 0x00FF00;
                    row_cache[fb_x] = 0xFF000000u | rb | g;
                }
            }
        }
        
        /* Blit the computed row `count` times to the framebuffer */
        uint32_t *dst = sf3000_fb_mem + (page_y_offset + fb_y) * dst_stride;
        for (int i = 0; i < count; i++) {
            uint32_t *d = dst;
            uint32_t *s = row_cache;
            // Manually unroll the 180-word copy (18 iterations of 10)
            // This aggressively bypasses generic libc memcpy overhead for uncached writes
            for (int k = 0; k < 18; k++) {
                *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
                *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
            }
            dst += dst_stride;
        }
        
        fb_y += count;
    }

    if (msg[0]) {
        /* Draw overlay text (FPS, CPU load, etc.) 
           px=10, py=10 is top-left in landscape */
        sf3000_text_native(10, 10, msg, 0xFFFFFFFFu, page_y_offset);
    }
    
    {
        struct fb_var_screeninfo vi = sf3000_vinfo;
        vi.xoffset = 0; vi.yoffset = page_y_offset;
        ioctl(sf3000_fb_fd, FBIOPAN_DISPLAY, &vi);

        static struct timeval last_tv = {0, 0};
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (last_tv.tv_sec != 0) {
            long long diff_us = (tv.tv_sec - last_tv.tv_sec) * 1000000LL + (tv.tv_usec - last_tv.tv_usec);
            if (diff_us > 0 && diff_us < 16666) {
                long long sleep_us = 16666 - diff_us;
                if (sleep_us > 2000) {
                    usleep(sleep_us - 2000);
                }
                while (1) {
                    gettimeofday(&tv, NULL);
                    diff_us = (tv.tv_sec - last_tv.tv_sec) * 1000000LL + (tv.tv_usec - last_tv.tv_usec);
                    if (diff_us >= 16666) break;
                }
            }
        }
        last_tv = tv;
    }
}

extern struct sf3000_btn sf3000_keymap[];
extern const int sf3000_keymap_count;

/* Native text: writes fontdata8x8 glyphs directly to fb0.
   physical_x = fb_y * 854/1014, physical_y = (179-fb_x) * 8/3.
   2 fb_y per font column (squarifies pixels), 1 fb_x per row. */
static void sf3000_text_native(int px, int py, const char *text, uint32_t color, int page_y_offset) {
    if (!sf3000_fb_mem) return;
    int dst_stride = (int)(sf3000_finfo.line_length / 4);
    
    int text_scale = 2; // 2x scale for readability
    int bg_padding = 4;
    
    int len = 0; while(text[len]) len++;
    int box_w_px = len * 8 * text_scale + bg_padding * 2;
    int box_h_px = 8 * text_scale + bg_padding * 2;
    
    // Draw solid black background
    for (int box_y = 0; box_y < box_h_px; box_y++) {
        int current_py = py + box_y;
        int fb_x = 179 - (current_py * 180 / 480);
        if (fb_x < 0 || fb_x > 179) continue;
        
        int fb_y_start = (px * 1014 / 854) + page_y_offset;
        int fb_y_end = ((px + box_w_px) * 1014 / 854) + page_y_offset;
        
        for (int fy = fb_y_start; fy < fb_y_end; fy++) {
            if (fy >= page_y_offset && fy < page_y_offset + 1280) {
                sf3000_fb_mem[fy * dst_stride + fb_x] = 0xFF000000u; 
            }
        }
    }
    
    // Draw scaled text
    px += bg_padding;
    py += bg_padding;
    for (int i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        for (int row = 0; row < 8; row++) {
            unsigned char fd = fontdata8x8[c * 8 + row];
            if (!fd) continue;
            
            for (int sr = 0; sr < text_scale; sr++) {
                int current_py = py + row * text_scale + sr;
                int fb_x = 179 - (current_py * 180 / 480);
                if (fb_x < 0 || fb_x > 179) continue;
                
                for (int col = 0; col < 8; col++) {
                    if (fd & (0x80 >> col)) {
                        int current_px = px + i * 8 * text_scale + col * text_scale;
                        int fb_y_start = (current_px * 1014 / 854) + page_y_offset;
                        int fb_y_end = ((current_px + text_scale) * 1014 / 854) + page_y_offset;
                        for (int fy = fb_y_start; fy < fb_y_end; fy++) {
                            if (fy >= page_y_offset && fy < page_y_offset + 1280) {
                                sf3000_fb_mem[fy * dst_stride + fb_x] = color;
                            }
                        }
                    }
                }
            }
        }
    }
}

static void sf3000_show_frame_native(const char **lines, int nlines) {
    if (!sf3000_fb_mem) return;
    int dst_stride = (int)(sf3000_finfo.line_length / 4);
    for (int fy = 0; fy < 1014; fy++)
        for (int fx = 0; fx < 180; fx++)
            sf3000_fb_mem[fy * dst_stride + fx] = 0xFF000000u;
    int py = 10;
    for (int i = 0; i < nlines; i++) {
        if (lines[i]) sf3000_text_native(20, py, lines[i], 0xFFFFFFFFu, 0);
        py += 30;
    }
    struct fb_var_screeninfo vi = sf3000_vinfo;
    vi.xoffset = vi.yoffset = 0;
    ioctl(sf3000_fb_fd, FBIOPAN_DISPLAY, &vi);
}

void sf3000_calibrate_input(void) {
    if (!sf3000_keys_ptr) return;

    static const char *bnames[] = {
        "UP","DOWN","LEFT","RIGHT","A","B","X","Y","L","R","L2","R2","SELECT","START" };
    const int N = 14;
    uint32_t captured[14];
    for (int i = 0; i < N; i++) captured[i] = 0xFF;

    for (int i = 0; i < N; i++) {
        char prompt[32], sub[32], hint[48];
        snprintf(prompt, sizeof(prompt), "PRESS: %s", bnames[i]);
        snprintf(sub,    sizeof(sub),    "Button %d of %d", i+1, N);
        snprintf(hint,   sizeof(hint),   "(hold 3s to skip)");

        /* flush held keys max 1s */
        for (int t = 0; t < 60 && (*sf3000_keys_ptr & 0xFFFF); t++) usleep(16000);

        uint32_t pressed = 0;
        int ticks = 0;
        const char *flines[3] = { prompt, sub, hint };
        while (!pressed && ticks < 180) {
            sf3000_show_frame_native(flines, 3);
            usleep(16000);
            ticks++;
            uint32_t cur = *sf3000_keys_ptr & 0xFFFF;
            if (cur) pressed = cur;
        }
        if (pressed) {
            captured[i] = pressed;
            for (int t = 0; t < 120 && (*sf3000_keys_ptr & pressed); t++) {
                sf3000_show_frame_native(flines, 3);
                usleep(16000);
            }
        }
    }

    /* Build summary lines and hold on screen until device rebooted */
    char lines[6][48];
    snprintf(lines[0], 48, "RESULTS (tell me these):");
    snprintf(lines[1], 48, "UP=%04X DN=%04X LT=%04X RT=%04X",
             captured[0], captured[1], captured[2], captured[3]);
    snprintf(lines[2], 48, "A=%04X B=%04X X=%04X Y=%04X",
             captured[4], captured[5], captured[6], captured[7]);
    snprintf(lines[3], 48, "L=%04X R=%04X L2=%04X R2=%04X",
             captured[8], captured[9], captured[10], captured[11]);
    snprintf(lines[4], 48, "SEL=%04X STA=%04X",
             captured[12], captured[13]);
    snprintf(lines[5], 48, "Reboot when done reading");

    for (int i = 0; i < 6; i++) fprintf(stderr, "%s\n", lines[i]);
    fflush(stderr);

    const char *slines[6] = { lines[0], lines[1], lines[2], lines[3], lines[4], lines[5] };
    while (1) {
        sf3000_show_frame_native(slines, 6);
        usleep(100000);
    }
}

void sf3000_load_keymap(void) {
    static const char *names[] = {
        "UP","DOWN","LEFT","RIGHT","A","B","X","Y","L","R","L2","R2","SELECT","START" };
    static const int retro_ids[] = { 4,5,6,7,8,0,9,1,10,11,12,13,2,3 };
    const int NMAP = 14;
    FILE *f = fopen("/mnt/sdcard/sf3000_keymap.txt", "r");
    if (!f) return;
    char name[16]; int bit;
    while (fscanf(f, "%15s %d", name, &bit) == 2) {
        for (int i = 0; i < NMAP; i++) {
            if (strcmp(name, names[i]) == 0) {
                sf3000_keymap[i].bit      = (uint8_t)bit;
                sf3000_keymap[i].retro_id = (uint8_t)retro_ids[i];
            }
        }
    }
    fclose(f);
    fprintf(stderr, "SF3000 input: loaded keymap\n");
}

void sf3000_fb_finish(void) {
    if (sf3000_use_hwdisp) {
        hwdisp_deinit();
        sf3000_use_hwdisp = 0;
    }
    if (sf3000_fb_mem) {
        munmap(sf3000_fb_mem, sf3000_fb_size);
        sf3000_fb_mem = NULL;
    }
    if (sf3000_fb_fd >= 0) {
        close(sf3000_fb_fd);
        sf3000_fb_fd = -1;
    }
    if (sf3000_rotated_src) {
        free(sf3000_rotated_src);
        sf3000_rotated_src = NULL;
        sf3000_rotated_capacity = 0;
    }
}

/* Restore fb0 to native portrait geometry (720x1280, 32bpp) using the existing
 * open fd.  Called from quit() before execl so the new process inherits a clean
 * display state.  Uses yres_virtual=2560 (2 pages) — safe when smem=11MB
 * (HCGE leftover), avoids FBIOPUT rejection that would occur with 5120. */
void sf3000_restore_fb0_geometry(void) {
    if (sf3000_fb_fd < 0) return;
    sf3000_dump_fb_state("restore/entry");
    struct fb_var_screeninfo vinfo;
    if (ioctl(sf3000_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) return;
    fprintf(stderr, "sf3000_restore_fb0_geometry: before: %ux%u(v%u) bpp=%u rot=%u\n",
            vinfo.xres, vinfo.yres, vinfo.yres_virtual, vinfo.bits_per_pixel, vinfo.rotate);
    /* Always FBIOPUT — even when geometry looks correct, the display
     * controller can still be routed to driver.so's buffer.  Pair with
     * /dev/dis ioctl to re-route display→fb0. */
    vinfo.xres = 720;
    vinfo.yres = 1280;
    vinfo.xres_virtual = 720;
    vinfo.yres_virtual = 2560; /* 2 pages; fits in HCGE-leftover smem (~11MB) */
    vinfo.xoffset = 0;
    vinfo.yoffset = 0;
    vinfo.bits_per_pixel = 32;
    vinfo.red    = (struct fb_bitfield){16, 8, 0};
    vinfo.green  = (struct fb_bitfield){ 8, 8, 0};
    vinfo.blue   = (struct fb_bitfield){ 0, 8, 0};
    vinfo.transp = (struct fb_bitfield){24, 8, 0};
    int r = ioctl(sf3000_fb_fd, FBIOPUT_VSCREENINFO, &vinfo);
    fprintf(stderr, "sf3000_restore_fb0_geometry: FBIOPUT ret=%d\n", r);
    /* Probe /dev/dis ioctl args — log all rets so we can see which combo
     * actually re-routes the display to fb0 after HCGE. */
    int dis = open("/dev/dis", O_RDWR);
    if (dis >= 0) {
        struct { int a, b, c; } b;
        b = (typeof(b)){1, 0, 0}; DBG("DBG restore: /dev/dis {1,0,0} ret=%d\n", ioctl(dis, 0xc00c0e0c, &b));
        b = (typeof(b)){0, 0, 0}; DBG("DBG restore: /dev/dis {0,0,0} ret=%d\n", ioctl(dis, 0xc00c0e0c, &b));
        b = (typeof(b)){1, 1, 0}; DBG("DBG restore: /dev/dis {1,1,0} ret=%d\n", ioctl(dis, 0xc00c0e0c, &b));
        b = (typeof(b)){2, 0, 0}; DBG("DBG restore: /dev/dis {2,0,0} ret=%d\n", ioctl(dis, 0xc00c0e0c, &b));
        b = (typeof(b)){1, 0, 0}; DBG("DBG restore: /dev/dis {1,0,0}-final ret=%d\n", ioctl(dis, 0xc00c0e0c, &b));
        close(dis);
    }
    sf3000_dump_fb_state("restore/post");
}

#endif // PLATFORM_SF3000
