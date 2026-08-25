/*
 * aleks_ra_badge.h -- optional achievement badge images.
 *
 * Ported from the final ALEKS TMC (port_ra_badge.h + switch_ra_badge.c): a
 * single-slot, fire-and-forget job queue on a background thread, a disk cache
 * under ra_badges/, a bounded download and a PNG signature check before the
 * bytes are trusted.
 *
 * ONE DELIBERATE CHANGE from the donor: the donor hands the caller raw PNG
 * bytes and decodes them in its renderer; here the worker decodes with
 * stb_image and hands over pixels, because the toast draws on the game thread
 * at the end of the frame and a PNG decode does not belong there.
 *
 * Everything is optional: a badge that never arrives simply means a toast
 * without a picture, never a missing toast.
 */
#pragma once
#include <stddef.h>

/* Queue one badge fetch.  Never performs I/O on the calling thread, and does
 * nothing if a fetch is already in flight. */
void AleksRA_Badge_Request(const char *badge_name, const char *url);

/*
 * Collect a decoded badge, if one finished.  Returns 1 and fills the outputs
 * on success; the pixels are RGBA8 and belong to the caller until it passes
 * them back to AleksRA_Badge_FreeReady.
 */
int AleksRA_Badge_TakeReady(const char **badge_name, void **pixels,
                            int *w, int *h);
void AleksRA_Badge_FreeReady(void *pixels);

/* Joins the worker before networking is torn down. */
void AleksRA_Badge_Shutdown(void);

/* Armed by the RA unlock event; drawn by aleks_ra_toast.c. */
void AleksRA_Toast(const char *title, int points, const char *badge_name);
