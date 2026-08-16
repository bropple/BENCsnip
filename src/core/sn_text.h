/*
 * BENCsnip - text on the picture
 *
 * Glyphs turned into pixels, here in the core rather than up in the interface,
 * because the whole reason sn::Renderer exists is that the preview and the
 * export ask one piece of code what the timeline looks like. Text drawn by
 * raylib would be text the exporter could not draw, and an export that differs
 * from the preview is the fault this program is arranged to prevent.
 *
 * The rasteriser is stb_truetype - one public-domain header, byte for byte the
 * copy raylib carries, so the outlines here and the outlines in the interface
 * come off the same code. It is the third-party dependency this program has
 * besides raylib and ffmpeg, and the README says so.
 *
 * ------------------------------------------------------------------
 *
 * Sizes and positions are fractions of the canvas, the way a track's transform
 * is, so that changing a project from 1080p to 4K moves nothing and resizes
 * nothing. A caption set at a tenth of the height is a tenth of the height at
 * both.
 */

#ifndef SN_TEXT_H
#define SN_TEXT_H

#include <cstdint>
#include <string>
#include <vector>

namespace sn {

/* 0xRRGGBBAA, so that a colour reads in the order it is written down and
 * "white" is 0xffffffff whichever end you start from. */
typedef uint32_t Rgba;

struct TextStyle {
    std::string text;

    /* The font file. Empty means the one compiled into this binary, which is
     * the only face guaranteed to be on every machine that runs this. A path
     * that has gone missing falls back to it - see textMissingFont. */
    std::string font;

    /* One line, ascender to descender, as a fraction of the canvas height.
     * 0.09 is about right for a caption. Not the cap height and not the point
     * size: it is the number stb_truetype scales a face by, which is the one
     * measurement every font agrees about. */
    double size = 0.09;

    /* Where it sits, on the same scale a track's picture uses: 0 centres it,
     * -1 puts it hard against the left or top edge, +1 against the right or
     * bottom. The box being placed is the text's own bounding box, so the
     * edges line up with the frame rather than with a glyph's ascender. */
    double x = 0.0, y = 0.5;

    /* Degrees, clockwise, about the centre of that box. */
    double rotation = 0.0;

    Rgba fill = 0xffffffffu;
    Rgba outline = 0x000000ffu;

    /* As a fraction of the size, so an outline stays in proportion to the
     * letters when the text is resized. 0 draws no outline at all. */
    double outlineWidth = 0.0;

    /* 0 left, 1 centre, 2 right. Only matters with more than one line. */
    int align = 1;

    /* Multiples of the line height. */
    double lineSpacing = 1.2;

    bool operator==(const TextStyle &o) const;
    bool operator!=(const TextStyle &o) const { return !(*this == o); }
};

/* The text as its own RGBA image, upright, with the outline already under it.
 *
 * Split out from drawText because this is the expensive half - laying out the
 * glyphs, rasterising them and growing the outline out of the result - and a
 * caller drawing the same caption on every frame of a ten second clip should
 * do it once. `of` and `forW`/`forH` are what it was built from, so a caller
 * holding one can tell whether it still applies.
 */
struct TextLayer {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;

    /* How tall this would be if it were one line.
     *
     * It is what the caption is placed by, rather than its whole height, so
     * that a second line - or a wider line gap - pushes downwards instead of
     * sliding the first line up to keep the block centred. Where the top of
     * the text is, is the thing somebody positioning a caption has in mind;
     * how far below it the rest of it reaches is not. */
    int anchorH = 0;

    TextStyle of;
    int forW = 0, forH = 0;

    bool valid() const { return w > 0 && h > 0 && !rgba.empty(); }
    bool matches(const TextStyle &st, int cw, int ch) const {
        return valid() && forW == cw && forH == ch && of == st;
    }
};

bool buildTextLayer(const TextStyle &st, int w, int h, TextLayer *out);

/* Put a built layer on the canvas: rotated, placed by the style's x and y,
 * and faded by `alpha` so that a clip's fade applies to text the way it
 * applies to a picture. */
void blitTextLayer(const TextLayer &layer, const TextStyle &st, uint8_t *rgba, int w,
                   int h, double alpha = 1.0);

/* Both at once, for a caller with nowhere to keep the layer - the tests, and
 * anything that draws a caption exactly once.
 *
 * False when there is nothing to draw - no text, no font, a size of zero -
 * rather than when something went wrong, because there is nothing a caller
 * could usefully do differently either way.
 */
bool drawText(const TextStyle &st, uint8_t *rgba, int w, int h, double alpha = 1.0);

/* The four corners of the text's box on a w x h canvas, rotation included,
 * clockwise from the top left, as x,y pairs in pixels.
 *
 * For the interface: it is what a selection outline is drawn around, what a
 * click is tested against and where the resize and rotate handles go. It is in
 * here rather than in the interface so that the box the mouse is offered is
 * the box the renderer actually fills, which two implementations of the same
 * layout would eventually disagree about.
 */
bool textBox(const TextStyle &st, int w, int h, double corners[8]);

/* Whether the font this style asks for could not be loaded and the embedded
 * face was used instead. For the interface to say so - a project that arrives
 * from another machine naming a font this one has not got should not silently
 * come out in a different typeface. */
bool textMissingFont(const TextStyle &st);

/* --- what fonts there are ---------------------------------------------- */

struct FontEntry {
    std::string name;   /* what to show: "DejaVu Sans Bold"          */
    std::string path;   /* what to load                              */
    int index = 0;      /* the face within a .ttc, 0 for a plain ttf */
};

/* Every font this machine has, from the places each platform keeps them. Read
 * from disk on the first call and remembered, because it means walking several
 * directories and opening every file in them to ask its name.
 *
 * No fontconfig, no CoreText, no GDI: the directories are well known on all
 * three platforms and this program is not in a position to add a dependency
 * per platform to fill in a menu. The cost is that a font installed somewhere
 * unusual is not listed - and can still be used, by giving its path. */
const std::vector<FontEntry> &systemFonts();

} /* namespace sn */

#endif /* SN_TEXT_H */
