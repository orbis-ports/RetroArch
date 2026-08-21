/* RetroArch - A frontend for libretro.
 *
 * The PS4's scan-out, behind six calls, so that sceVideoOut appears in exactly one file.
 *
 * The sequence this wraps is not a new idea: it is a transcription of what
 * ~/src/ps4doom/platform/doomgeneric_ps4.c does, which has run on hardware. Two steps in
 * it are counter-intuitive and both were learned the hard way there:
 *
 *   1. THE FRAMEBUFFERS MUST BE DIRECT MEMORY. sceVideoOutRegisterBuffers refuses
 *      malloc'd memory on a real console with 0x80290013. It accepts it under the
 *      emulator, which is exactly how that costs a day.
 *   2. THE FLIP EVENT QUEUE IS NOT OPTIONAL. Without sceVideoOutAddFlipEvent, submitted
 *      flips are not processed, OrbisVideoOutFlipStatus::flipArg never advances, and any
 *      loop that waits for a frame to be taken runs at minutes per frame.
 *
 * ⚠ ONE PIXEL FORMAT. The SDK names exactly one - ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB
 * (orbis/_types/video.h) - and its bytes are R,G,B,A low to high, i.e. 0xAABBGGRR as a
 * little-endian uint32. That is RetroArch's SCALER_FMT_ABGR8888 exactly, so the frontend's
 * own scaler converts into it with no swizzle pass of ours: conv_argb8888_abgr8888 and
 * conv_rgb565_abgr8888 already exist (libretro-common/gfx/scaler/scaler.c:185,219).
 */

#ifndef PS4_VIDEO_OUT_H__
#define PS4_VIDEO_OUT_H__

#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct ps4_video_out ps4_video_out_t;

/* Opens video-out and registers `buffers` linear framebuffers of width x height.
 * Returns NULL and logs the call that refused - a display that cannot be opened must fail
 * loudly here rather than present into nothing every frame afterwards. */
ps4_video_out_t *ps4_video_out_open(unsigned width, unsigned height, unsigned buffers);

void ps4_video_out_close(ps4_video_out_t *vo);

/* The buffer to draw into: the one that is NOT on screen. Its rows are
 * ps4_video_out_pitch_px() pixels apart, which equals the width for now but is asked for
 * rather than assumed, because a pitch the hardware chose is how a picture goes diagonal. */
uint32_t *ps4_video_out_backbuffer(ps4_video_out_t *vo);
unsigned  ps4_video_out_pitch_px(const ps4_video_out_t *vo);
void      ps4_video_out_size(const ps4_video_out_t *vo, unsigned *width, unsigned *height);

/* Zeroes EVERY buffer in place, without submitting anything. The buffers rotate, so a
 * border cleared only in the one being drawn into reappears when the other comes round -
 * as a band of the previous geometry that no frame ever overwrites. Doing it through flips
 * instead would put a black frame on screen for each buffer. */
void ps4_video_out_clear(ps4_video_out_t *vo);

/* Hands the back buffer to the display and rotates. `wait` blocks until the flip has been
 * taken, which is what throttles the frontend to the display's rate; with `wait` false the
 * caller runs ahead and the display shows whatever was most recently completed. */
bool ps4_video_out_flip(ps4_video_out_t *vo, bool wait);

RETRO_END_DECLS

#endif
