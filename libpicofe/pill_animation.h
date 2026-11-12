/*
 * pill_animation.h - animated sliding highlight ("pill") for the picoarch menu.
 *
 * NOTE: this file is missing from the upstream tree. Commit f77a0d5 ("Fix
 * PicoArch pill menus and option scrolling") added  #include "pill_animation.h"
 * to libpicofe/menu.h and uses the type/functions below, but never committed
 * the file itself - so every fresh clone fails with:
 *   libpicofe/menu.h:14:28: fatal error: pill_animation.h: No such file
 * This is a faithful reconstruction of the interface menu.c uses:
 *
 *   static pill_animation selection_animation;
 *   pill_animation_reset(&a, target_y);                       - snap to a row
 *   pill_animation_start(&a, a.current_y, target_y, 3);       - begin slide
 *   y = pill_animation_step(&a);                              - per-frame y
 *   pill_animation_active(&a);                                - still moving?
 *
 * The pill eases from its previous row to the newly selected row with an
 * ease-out quadratic curve in pure integer math (no float, no libm), and
 * always lands EXACTLY on the target row.
 */
#ifndef PILL_ANIMATION_H
#define PILL_ANIMATION_H

typedef struct {
	int current_y;   /* pixel y the pill is drawn at this frame */
	int from_y;      /* pixel y the slide started from          */
	int to_y;        /* pixel y of the selected row             */
	int frames;      /* total frames the slide takes            */
	int t;           /* frames elapsed since the slide started  */
	int active;      /* 1 while the pill is still moving        */
} pill_animation;

/* Snap the pill to y immediately (menu just (re)entered / state reset). */
static inline void pill_animation_reset(pill_animation *a, int y)
{
	a->current_y = y;
	a->from_y    = y;
	a->to_y      = y;
	a->frames    = 0;
	a->t         = 0;
	a->active    = 0;
}

/* Begin a slide from from_y to to_y over the given number of frames. */
static inline void pill_animation_start(pill_animation *a, int from_y,
                                        int to_y, int frames)
{
	a->from_y = from_y;
	a->to_y   = to_y;
	a->frames = frames > 0 ? frames : 1;
	a->t      = 0;
	a->active = (a->to_y != a->from_y);
	if (!a->active)
		a->current_y = a->to_y;
}

/* Advance one frame; return the pixel y to draw the pill at. */
static inline int pill_animation_step(pill_animation *a)
{
	if (a->active) {
		a->t++;
		if (a->t >= a->frames) {
			a->current_y = a->to_y;      /* land exactly on the row */
			a->active    = 0;
		} else {
			/* ease-out quadratic: e(t) = (2*T*t - t*t) / (T*T) */
			int d    = a->to_y - a->from_y;
			int T    = a->frames;
			int t    = a->t;
			int num  = 2 * T * t - t * t;
			a->current_y = a->from_y + (d * num) / (T * T);
		}
	}
	return a->current_y;
}

/* 1 while the pill is still sliding (menus redraw until it settles). */
static inline int pill_animation_active(const pill_animation *a)
{
	return a->active;
}

#endif /* PILL_ANIMATION_H */
