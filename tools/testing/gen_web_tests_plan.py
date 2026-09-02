#!/usr/bin/env python3
"""
Turn Chromium's own web tests into a comparison plan.

    tools/testing/gen_web_tests_plan.py <web_tests-dir> <suite>... > out.plan

The tree under third_party/blink/web_tests is the corpus Chromium's authors
wrote to exercise every text code path they ever fixed, so it reaches
behavior no saved page does. Symlink the web_tests directory into the served
corpus as wt/, and its resources/ and external/wpt/fonts/ directories at the
corpus root, so the tests' absolute references resolve on both sides.

The plan header still has to come from an existing plan; this emits page
lines only, one per test file, so:

    head -5 parity.plan > wpt.plan
    tools/testing/gen_web_tests_plan.py .../web_tests \\
        external/wpt/css/css-fonts external/wpt/css/css-text >> wpt.plan

Skipped outright: -expected files, and the tests that render differently on
purpose from one run to the next, which are listed in SKIP and SKIP_NAMES
with the reason.

An `accept` line is emitted ahead of the pages, naming the file of paths whose
difference is the toolkit drawing its own widgets. Those cells are still swept
and printed, marked, and kept out of the totals that a regression would show
in; see widget-chrome.accept for what is in it and why.
"""

import os
import sys

# Tests whose rendering is a function of time, so two captures disagree
# without either side being wrong.
SKIP = {
    # font-display staging: the test is which swap phase the capture lands in.
    "font-display/",
    # Animations sample whatever frame the capture reaches.
    "animations/",
}

# The same, for tests a directory does not gather. Matched against the file
# name, so a substring covers a whole family of them.
SKIP_NAMES = {
    # <marquee> scrolls on a timer of its own, so one capture catches the text
    # part way across and the other at the start.
    "marquee",
    # A <progress> with no value is indeterminate, and the theme animates the
    # pulse across it. This page draws two, and the pair lands at a different
    # phase in every capture.
    "pseudo-replaced-elements",
}


# Written ahead of the pages, so a generated plan carries its own accepted
# cells and a sweep of it needs no extra argument to stay readable.
ACCEPT = "widget-chrome.accept"


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip())
    root = sys.argv[1]
    print("accept %s" % ACCEPT)
    for suite in sys.argv[2:]:
        for base, _, names in sorted(os.walk(os.path.join(root, suite))):
            rel_base = os.path.relpath(base, root)
            if any(s.rstrip("/") in rel_base.split(os.sep) for s in SKIP):
                continue
            for name in sorted(names):
                if not name.endswith(".html") or "expected" in name:
                    continue
                if any(s in name for s in SKIP_NAMES):
                    continue
                print("page wt/%s 0" % os.path.join(rel_base, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
