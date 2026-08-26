#!/usr/bin/env python3
"""
Score one Linux screenshot against a Windows reference, on one line.

    tools/testing/score_blend.py windows.png linux.png

Aligns on the reference page's corner marks, then searches a small offset
window and reports the best. The search matters: a one-pixel disagreement in
where the text sits would otherwise dominate the pixel numbers and hide the
thing being tuned, which is how heavy the glyphs are, not where they are.

Only ink pixels count. The page is mostly white, and including the white
would report a flattering number no matter how wrong the text was.
"""

import sys

import numpy as np
from PIL import Image


def crop(path, dx=0, dy=0, w=960, h=620):
    pixels = np.asarray(Image.open(path).convert("RGB")).astype(int)
    dark = (pixels.sum(2) < 120)          # the four pure-black corner marks
    ys, xs = np.nonzero(dark)
    x, y = xs.min() + dx, ys.min() + dy
    return pixels[y:y + h, x:x + w]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    reference = crop(sys.argv[1])
    ink = (reference.sum(2) < 720)

    best = None
    for dy in range(-3, 4):
        for dx in range(-3, 4):
            candidate = crop(sys.argv[2], dx, dy)
            if candidate.shape != reference.shape:
                continue
            diff = np.abs(reference - candidate)
            mean = diff[ink].mean()
            if best is None or mean < best[0]:
                best = (mean, dx, dy, diff)

    mean, dx, dy, diff = best
    exact = 100.0 * (diff.sum(2) == 0)[ink].mean()
    gap = abs(diff[:, :, 0][ink].mean() - diff[:, :, 2][ink].mean())
    lighter = "lighter" if crop(sys.argv[2], dx, dy)[ink].mean() > reference[ink].mean() else "heavier"
    print("mean|diff| %6.2f  exact %5.2f%%  offset %+d,%+d  R-B gap %4.2f  (%s than Windows)"
          % (mean, exact, dx, dy, gap, lighter))


if __name__ == "__main__":
    main()
