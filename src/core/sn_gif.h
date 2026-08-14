/*
 * BENCsnip - the palette a GIF needs
 *
 * A GIF frame is 256 colours and an index per pixel, and something has to
 * choose those 256 colours. ffmpeg can do it - palettegen and paletteuse are
 * filters - but this program links no libavfilter: it does its own scaling,
 * mixing and compositing, and a filter graph carried for one format would be
 * several megabytes in every copy of the binary for a feature most exports
 * never touch.
 *
 * So the choice is made here. Without it the encoder is handed rgb8, which is
 * a fixed three-three-two palette - eight levels of red, eight of green, four
 * of blue - and everything that is not a primary colour arrives as a
 * near-miss. Skin goes grey-pink, a sunset goes in bands, and a BENCO green
 * near-black goes black.
 *
 * Two passes, which is what any tool that produces a decent GIF does:
 *
 *   Look at the footage first, and pick colours that suit it. A clip of
 *   mostly sky wants most of its palette in blues; the same 256 entries
 *   spread evenly over the cube would spend two hundred of them on colours
 *   that never appear.
 *
 *   Then map every pixel to the nearest of them, with a dither so that the
 *   colours in between are mixed rather than rounded.
 */

#ifndef SN_GIF_H
#define SN_GIF_H

#include <cstddef>
#include <cstdint>

namespace sn {

struct Palette {
    uint8_t rgb[256][3];
    int n = 0;

    /* Which palette this is, counting from the first one ever built. The
     * lookup cache in quantise() has to know when it is looking at a
     * different set of colours, and neither the address nor the size can tell
     * it: a second export puts its palette on the same piece of stack as the
     * first, and both of them are 256 colours. Without a serial the second
     * GIF comes out in the first one's colours. */
    uint32_t serial = 0;
};

/* Choose `want` colours (at most 256) to represent these pixels.
 *
 * `rgba` is a run of frames one after another - the caller renders a handful
 * spread across the export rather than all of them, because a palette built
 * from sixteen frames of a clip is indistinguishable from one built from four
 * hundred and takes a fortieth of the time.
 */
void buildPalette(const uint8_t *rgba, size_t pixels, int want, Palette *out);

/* Map a frame onto the palette, writing one index per pixel.
 *
 * Ordered dithering rather than error diffusion: a Bayer pattern is fixed, so
 * the same colour lands on the same index in every frame, and a GIF is
 * compressed between frames as well as within them. Floyd-Steinberg looks
 * slightly better on a still and makes an animation noticeably larger,
 * because the noise it spreads is different in every frame.
 */
void quantise(const uint8_t *rgba, int w, int h, const Palette &pal, uint8_t *indices,
              int indexStride, bool dither);

} /* namespace sn */

#endif /* SN_GIF_H */
