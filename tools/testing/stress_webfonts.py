#!/usr/bin/env python3
"""
stress_webfonts.py - drive a browser through web fonts and watch what the shim
keeps.

    tools/testing/gen_webfont_stress.py /tmp/wfA --fonts 90 --pages 9
    cd /tmp/wfA && python3 -m http.server 8140 --bind 127.0.0.1 &
    CLEARTYPE_LOG=/tmp/ct CLEARTYPE_CENSUS_SECONDS=3 \
      tools/testing/run_parity_firefox.sh start --url http://127.0.0.1:8140/
    tools/testing/stress_webfonts.py /tmp/ct http://127.0.0.1:8140/ [more urls]

Every web font a page loads reaches the shim as FT_New_Memory_Face, and what
DirectWrite is handed that way it keeps for the life of the process - see the
note above GetMemoryLoader in cleartype/src/freetype.cpp. So the question is not
whether the count rises but whether it stops: it must converge on the number
of *distinct* fonts and then hold while the same pages are visited again.

The shim answers it directly. With CLEARTYPE_LOG set it writes a census of
every side table it owns, and CLEARTYPE_CENSUS_SECONDS sets how often; this
reads the newest one from whichever process is doing the rasterizing. Pages
are scrolled rather than only loaded, because a font below the fold is never
painted and never reaches the shim at all.
"""

import argparse
import glob
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from marionette import Marionette


def newest_census(log_prefix):
    """The newest census line, and the pid that wrote it."""
    newest = None
    for path in glob.glob(log_prefix + ".*"):
        try:
            with open(path, errors="replace") as handle:
                lines = [l for l in handle if "table census" in l]
        except OSError:
            continue
        if lines:
            newest = (path.rsplit(".", 1)[1], lines[-1].split("census ", 1)[1].strip())
    return newest


def rss_kb(pid):
    try:
        for line in open("/proc/%s/status" % pid):
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except OSError:
        pass
    return 0


def pages_at(url):
    """The pages gen_webfont_stress.py wrote, read out of its index."""
    with urllib.request.urlopen(url.rstrip("/") + "/index.html", timeout=20) as fh:
        body = fh.read().decode("utf-8", "replace")
    return [url.rstrip("/") + "/" + name for name in re.findall(r"href='([^']+)'", body)]


def scroll(m: Marionette):
    try:
        height = m.script("return document.documentElement.scrollHeight;")
        step = max(300, int(m.script("return window.innerHeight;") * 0.9))
    except Exception:
        return
    y = 0
    while y < height:
        try:
            m.script("window.scrollTo(0,%d); return document.documentElement.offsetHeight;" % y)
        except Exception:
            return
        time.sleep(0.05)
        y += step


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log_prefix", help="the CLEARTYPE_LOG the browser was started with")
    ap.add_argument("url", nargs="+", help="one or more gen_webfont_stress.py roots")
    ap.add_argument("--rounds", type=int, default=8)
    ap.add_argument("--port", type=int, default=2828)
    args = ap.parse_args()

    sets = [pages_at(u) for u in args.url]
    total = sum(len(s) for s in sets)
    m = Marionette("127.0.0.1", args.port, timeout=180)
    m.start("content")

    started = time.time()
    loads = 0
    print("%-6s %-7s %-9s %s" % ("round", "loads", "rss(kB)", "census"))
    for r in range(args.rounds):
        for pages in sets:
            for page in pages:
                m.call("WebDriver:Navigate", {"url": "%s?r=%d" % (page, r)})
                time.sleep(0.4)
                scroll(m)
                loads += 1
        got = newest_census(args.log_prefix)
        if got:
            print("%-6s %-7s %-9s %s" % (r + 1, loads, rss_kb(got[0]), got[1]))
            sys.stdout.flush()

    m.call("WebDriver:Navigate", {"url": "about:blank"})
    time.sleep(10)
    got = newest_census(args.log_prefix)
    if got:
        print("%-6s %-7s %-9s %s" % ("final", loads, rss_kb(got[0]), got[1]))
    print("%d pages, %d loads, %.0fs" % (total, loads, time.time() - started))


if __name__ == "__main__":
    main()
