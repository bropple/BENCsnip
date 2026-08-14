#!/usr/bin/env python3
"""Write a variant of the icon artwork with named parts taken out.

    tools/icon-variant.py SOURCE.svg OUT.svg [--no-strip]

The background rect always goes: an icon needs to sit on a dark taskbar, a
light desktop and the macOS Dock, and a flat plate behind it only looks
deliberate on one of them.

--no-strip also removes the film strip, for the sizes where it stops being a
film strip and becomes four grey smudges across the artwork. Simplifying
rather than shrinking is what icon sets do at 16 px, and the star icon this
replaced did the same thing with its visor.

Parts are matched on what they are rather than on where they are in the file,
so re-exporting the artwork from a drawing program does not silently turn this
into a no-op:

    background   the one rect filling the whole canvas in #b1b5b8
    film strip   the group rotated -38 degrees, which nothing else is

If either stops matching, this says so and exits non-zero rather than writing
a variant that is identical to the source.
"""

import sys
import xml.etree.ElementTree as ET

NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", NS)

BACKGROUND_FILL = "#b1b5b8"
STRIP_TRANSFORM = "rotate(-38)"


def remove(root, predicate):
    """Drop every element the predicate likes. Returns how many went."""
    n = 0
    for parent in root.iter():
        for child in list(parent):
            if predicate(child):
                parent.remove(child)
                n += 1
    return n


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    src, out = argv[1], argv[2]
    no_strip = "--no-strip" in argv[3:]

    tree = ET.parse(src)
    root = tree.getroot()

    gone = remove(
        root,
        lambda e: e.tag == f"{{{NS}}}rect"
        and (e.get("fill") or "").lower() == BACKGROUND_FILL,
    )
    if gone == 0:
        print(f"{src}: no {BACKGROUND_FILL} background rect to remove", file=sys.stderr)
        return 1

    if no_strip:
        gone = remove(
            root,
            lambda e: e.tag == f"{{{NS}}}g"
            and STRIP_TRANSFORM in (e.get("transform") or ""),
        )
        if gone == 0:
            print(f"{src}: no group transformed by {STRIP_TRANSFORM}", file=sys.stderr)
            return 1

    tree.write(out, xml_declaration=True, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
