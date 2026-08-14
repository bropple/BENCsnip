/*
 * BENCsnip - S. Tarr
 *
 * BENCO's audio/visual person, and so the one who turns up in an editor. He is
 * drawn from the roster geometry in sn_gui.cpp; what is here is the small
 * amount of behaviour on top of it.
 *
 * The style guide says no decorative animation, and this obeys it: the mark is
 * still, except while an export is running, where the slow turn is a progress
 * indicator rather than a flourish - it is how you can tell across the room
 * that the program has not hung.
 */

#include "sn_app.h"

#include <cmath>

namespace sn {

void tarr(App &a, Vector2 center, float radius, TarrMood mood)
{
    float rot = 0.0f;
    if (mood == TARR_BUSY) rot = (float)std::fmod(GetTime() * 40.0, 360.0) * DEG2RAD;

    sn_star(center, radius, rot);

    /* The visor stripe is the expression. Wider and level is the ordinary
     * face; a shorter one reads as a wince, which is what an error gets. The
     * base mark is already drawn, so this only paints over the stripe. */
    if (mood == TARR_SORRY) {
        Rectangle strip = {center.x - radius * 0.32f, center.y + radius * 0.042f,
                           radius * 0.64f, radius * 0.168f};
        DrawRectangleRec(Rectangle{center.x - radius * 0.737f * 0.5f,
                                   center.y + radius * 0.042f, radius * 0.737f,
                                   radius * 0.168f},
                         SN_VISOR);
        DrawRectangleRec(strip, SN_ALERT);
    } else if (mood == TARR_HAPPY) {
        /* A glint on the visor. One small light rectangle, which at this
         * scale is the whole difference between "working" and "pleased". */
        DrawRectangleRec(Rectangle{center.x + radius * 0.30f, center.y - radius * 0.02f,
                                   radius * 0.14f, radius * 0.10f},
                         Color{0xff, 0xff, 0xff, 200});
    }
}

const char *tarrLine(int which)
{
    /* Deadpan, unbothered, and never more than a line. The point of these is
     * that an empty window says what to do next without a tutorial. */
    static const char *lines[] = {
        "Nothing on the timeline.",
        "Drag something in from the left.",
        "The bin is on the left. The timeline is here.",
        "Ready when you are.",
        "No footage. No opinion.",
    };
    const int n = (int)(sizeof lines / sizeof lines[0]);
    if (which < 0) which = 0;
    return lines[which % n];
}

} /* namespace sn */
