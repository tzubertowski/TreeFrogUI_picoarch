/* No-op replacement for the proprietary display driver during user-mode QEMU
 * memory profiles. PicoArch still executes its real R36SX render path. */
void *g_render;

int video_drivers_init(void) { return 1; }
void video_driver_deinit(void) { }
int video_driver_disp_frame(void *pixels, int width, int height, int pitch)
{
	(void)pixels; (void)width; (void)height; (void)pitch;
	return 0;
}
void fbdev_video_aspect_ratio(int fit) { (void)fit; }
int video_driver_setmode(int a, int b) { (void)a; (void)b; return 0; }
void fbdev_set_enhance(int a, int b, int c, int d, int e)
{
	(void)a; (void)b; (void)c; (void)d; (void)e;
}
