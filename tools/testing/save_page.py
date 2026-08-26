#!/usr/bin/env python3
"""
save_page.py - fetch a page and make it render the same twice, offline.

    tools/testing/save_page.py <url> <dir>

A parity comparison needs both machines looking at the same bytes. A live URL
is not that: the page changes, the CDN answers differently, and a slow asset
turns into a missing one on whichever side was unlucky. So the page is pulled
once into a directory that can be served to both.

The browser does the pulling. Firefox's own "Save Page As -> Web Page,
complete" is nsIWebBrowserPersist, driven here through Marionette with the
file picker bypassed: it fetches subresources in parallel, mostly out of the
cache it filled while rendering, and saves the DOM as rendered rather than the
HTML as served. Where no browser is found, or one is found but never answers
on Marionette, it falls back to fetching the HTML, inlining every stylesheet,
rewriting each url() and downloading every <img>, one request at a time.

What is then done to what it saved, so that both routes end in the same kind
of directory:

  * every <script> is dropped. A page that fetches and mutates itself does not
    render the same twice, and the comparison is of two rasterizers, not of two
    network conditions. Anchor hrefs are left pointing at the real site: they
    are navigation targets, never fetched while rendering;
  * anything still pointing off the machine after the save - a tracking pixel
    the page inserted from script, say - is dropped rather than left to be
    fetched at render time;
  * the assets directory is renamed to a/ and the page to index.html, which is
    the layout the rest of tools/testing expects;
  * <base> goes, because a saved page must not resolve anything remotely.
"""

import argparse
import hashlib
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from urllib.parse import unquote, urljoin, urlparse
from urllib.request import Request, urlopen
from urllib.error import HTTPError

from bs4 import BeautifulSoup

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def save_by_fetching(base, out_dir):
    user_agent = "Mozilla/5.0 (X11; Linux x86_64; rv:154.0) Gecko/20100101 Firefox/154.0"
    os.makedirs(os.path.join(out_dir, "a"), exist_ok=True)
    cache = {}

    def fetch(url):
        """One polite request, retried on 429. A site that rate-limits a burst
        answers with a page missing images, which is a different page."""
        if url in cache:
            return cache[url]
        delay = 1.0
        for attempt in range(5):
            try:
                time.sleep(0.4)
                with urlopen(Request(url, headers={"User-Agent": user_agent}), timeout=60) as r:
                    cache[url] = (r.read(), r.headers.get("Content-Type", ""))
                    return cache[url]
            except HTTPError as e:
                if e.code in (429, 503) and attempt < 4:
                    time.sleep(delay); delay *= 2; continue
                print("  MISS", url[:80], e); break
            except Exception as e:
                print("  MISS", url[:80], e); break
        cache[url] = (None, "")
        return cache[url]

    def ext_for(url, ctype):
        for e in (".svg", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".woff2", ".woff", ".ttf"):
            if url.lower().split("?")[0].endswith(e):
                return e
        return {"image/svg+xml": ".svg", "image/png": ".png", "image/jpeg": ".jpg",
                "image/gif": ".gif", "image/webp": ".webp",
                "font/woff2": ".woff2"}.get(ctype.split(";")[0].strip(), ".bin")

    def save(url):
        """Fetch url into a/ and return its local relative path, or None."""
        data, ctype = fetch(url)
        if data is None:
            return None
        name = hashlib.sha256(url.encode()).hexdigest()[:16] + ext_for(url, ctype)
        open(os.path.join(out_dir, "a", name), "wb").write(data)
        return "a/" + name

    def localize_css(text, css_url):
        def repl(m):
            raw = m.group(1).strip().strip('"\'')
            if raw.startswith("data:") or not raw:
                return m.group(0)
            local = save(urljoin(css_url, raw))
            return "url(%s)" % (("../" + local) if local else raw)
        return re.sub(r"url\(([^)]*)\)", repl, text)

    page_raw, page_ctype = fetch(base)
    if page_raw is None:
        sys.exit("could not fetch " + base)
    open(os.path.join(out_dir, "raw.html"), "wb").write(page_raw)
    soup = BeautifulSoup(page_raw, "html.parser")

    # Carry the charset into the document.
    #
    # A server may declare it only in the Content-Type header, and a page that
    # relies on that has no <meta charset> of its own. Saved to a file and served
    # by something that does not repeat the header, it decodes as Latin-1 - and
    # the failure is quiet: the page still renders, in mojibake, and a comparison
    # of two machines rendering the same mojibake looks like a font problem.
    declared = ""
    for part in page_ctype.split(";"):
        if part.strip().lower().startswith("charset="):
            declared = part.split("=", 1)[1].strip().strip('"\'')
    if soup.find("meta", attrs={"charset": True}):
        declared = ""
    for meta in soup.find_all("meta"):
        if (meta.get("http-equiv") or "").lower() == "content-type":
            declared = ""
    if declared:
        head = soup.find("head")
        if head is None:
            head = soup.new_tag("head")
            (soup.find("html") or soup).insert(0, head)
        tag = soup.new_tag("meta")
        tag["charset"] = declared
        head.insert(0, tag)
        print("  added <meta charset=%s>, which the server sent only as a header"
              % declared)

    for tag in soup.find_all("script"):
        tag.decompose()
    for tag in soup.find_all("link"):
        rel = " ".join(tag.get("rel") or [])
        if "stylesheet" in rel:
            if not tag.get("href"):        # a sheet script was going to fill in
                tag.decompose(); continue
            href = urljoin(base, tag["href"])
            sheet_data, _ = fetch(href)
            if sheet_data is None:
                tag.decompose(); continue
            style = soup.new_tag("style")
            style.string = localize_css(sheet_data.decode("utf-8", "replace"), href)
            tag.replace_with(style)
        elif rel and rel not in ("canonical",):
            tag.decompose()

    # <style> blocks already in the document can reference images too.
    for tag in soup.find_all("style"):
        if tag.string:
            tag.string = localize_css(tag.string, base)

    count = 0
    for tag in soup.find_all(["img", "source"]):
        if tag.get("src"):
            local_path = save(urljoin(base, tag["src"]))
            if local_path:
                tag["src"] = local_path; count += 1
            else:
                tag.decompose(); continue
        if tag.get("srcset"):
            parts = []
            for item in tag["srcset"].split(","):
                bits = item.strip().split()
                if not bits:
                    continue
                local_path = save(urljoin(base, bits[0]))
                if local_path:
                    parts.append(" ".join([local_path] + bits[1:]))
            tag["srcset"] = ", ".join(parts) if parts else ""
            if not parts:
                del tag["srcset"]

    for tag in soup.find_all("base"):
        tag.decompose()

    open(os.path.join(out_dir, "index.html"), "w", encoding="utf-8").write(str(soup))
    print("saved", os.path.join(out_dir, "index.html"),
          os.path.getsize(os.path.join(out_dir, "index.html")), "bytes;",
          len(os.listdir(os.path.join(out_dir, "a"))), "assets;", count, "images")


# ---------------------------------------------------------------------------
# The browser route
# ---------------------------------------------------------------------------

def find_firefox():
    """The same search run_parity_firefox.sh does, in the same order."""
    for candidate in ("firefox", "firefox-esr", "librewolf"):
        found = shutil.which(candidate)
        if found:
            return found
    for path in ("/usr/lib/firefox/firefox", "/opt/firefox/firefox",
                 os.path.expanduser("~/.local/share/firefox/firefox")):
        if os.path.exists(path):
            return path
    return None


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    return port


def tidy(saved_html, assets_dir, out_dir):
    """Make the browser's output into the directory the fetch route produces.

    The two routes disagree about one thing on purpose: this one saves the DOM
    as rendered, so anything script inserted is in it. Stripping the scripts
    afterwards leaves that content in place but stops it running again, which
    is what "renders the same twice" needs.
    """
    with open(saved_html, encoding="utf-8", errors="replace") as handle:
        raw = handle.read()
    with open(os.path.join(out_dir, "raw.html"), "w", encoding="utf-8") as handle:
        handle.write(raw)

    soup = BeautifulSoup(raw, "html.parser")
    scripts = len(soup.find_all("script"))
    for tag in soup.find_all("script"):
        tag.decompose()
    for tag in soup.find_all("base"):
        tag.decompose()

    # Whatever the save could not localize would be fetched at render time,
    # which is the one thing an offline copy must not do.
    remote = 0
    for tag in soup.find_all(["img", "source", "video", "audio", "iframe", "embed"]):
        for attr in ("src", "srcset", "poster", "data"):
            value = tag.get(attr)
            if value and re.match(r"https?://", value.strip()):
                tag.decompose()
                remote += 1
                break

    # The rest of tools/testing serves these directories and opens index.html,
    # with the assets under a/.
    assets_out = os.path.join(out_dir, "a")
    if os.path.isdir(assets_dir):
        if os.path.isdir(assets_out):
            shutil.rmtree(assets_out)
        shutil.move(assets_dir, assets_out)
    # The directory Firefox created is named after the page title, so it holds
    # spaces and punctuation, and every attribute pointing into it is written
    # percent-encoded. Decoding each attribute is what makes the two forms
    # comparable; replacing the raw name in the serialized HTML matches nothing.
    old_prefix = os.path.basename(assets_dir) + "/"

    def localize(url):
        decoded = unquote(url)
        return "a/" + decoded[len(old_prefix):] if decoded.startswith(old_prefix) else url

    for tag in soup.find_all(["img", "source", "video", "audio", "link", "image", "use"]):
        for attr in ("src", "href", "poster", "data", "xlink:href"):
            if tag.get(attr):
                tag[attr] = localize(tag[attr])
        if tag.get("srcset"):
            candidates = []
            for candidate in tag["srcset"].split(","):
                parts = candidate.split()
                if parts:
                    candidates.append(" ".join([localize(parts[0])] + parts[1:]))
            tag["srcset"] = ", ".join(candidates)
    html = str(soup)

    index = os.path.join(out_dir, "index.html")
    with open(index, "w", encoding="utf-8") as handle:
        handle.write(html)
    os.remove(saved_html)

    assets = len(os.listdir(assets_out)) if os.path.isdir(assets_out) else 0
    print("saved %s %d bytes; %d assets; dropped %d script(s) and %d that stayed remote"
          % (index, os.path.getsize(index), assets, scripts, remote))


def save_by_browser(url, out_dir):
    browser = find_firefox()
    if browser is None:
        return False

    from marionette import Marionette

    port = free_port()
    profile = tempfile.mkdtemp(prefix="save-page-")
    # Marionette's port is a pref, not a command-line flag, so it has to be in
    # the profile before the browser starts. A free one, so this never
    # collides with a browser the caller is already running.
    with open(os.path.join(profile, "user.js"), "w", encoding="utf-8") as handle:
        handle.write('user_pref("marionette.port", %d);\n' % port)
        handle.write('user_pref("browser.shell.checkDefaultBrowser", false);\n')
        handle.write('user_pref("datareporting.policy.dataSubmissionEnabled", false);\n')
    # -headless needs no display, and CLEARTYPE=0 keeps this library out of a
    # browser whose only job here is to fetch bytes.
    env = dict(os.environ, CLEARTYPE="0")
    env.pop("LD_PRELOAD", None)
    proc = subprocess.Popen(
        [browser, "-headless", "-marionette", "-remote-allow-system-access",
         "-profile", profile, "-no-remote", "about:blank"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        client = None
        for _ in range(60):
            time.sleep(1)
            try:
                client = Marionette("127.0.0.1", port, timeout=180)
                client.start("chrome")
                break
            except Exception:
                client = None
        if client is None:
            return False

        client.script("""
            Services.prefs.setIntPref("browser.download.folderList", 2);
            Services.prefs.setCharPref("browser.download.dir", arguments[0]);
            Services.prefs.setBoolPref("browser.download.useDownloadDir", true);
            Services.prefs.setBoolPref(
                "browser.download.always_ask_before_handling_new_types", false);
        """, [out_dir])

        client.call("Marionette:SetContext", {"value": "content"})
        client.call("WebDriver:Navigate", {"url": url})
        # The save takes the DOM as it stands, so wait for the page to have
        # stopped changing it: parsing done, then the fonts it asked for.
        try:
            client.script("return document.fonts.ready.then(() => true);")
        except Exception:
            pass

        before = set(os.listdir(out_dir))
        client.call("Marionette:SetContext", {"value": "chrome"})
        client.script("saveBrowser(gBrowser.selectedBrowser, true);")

        saved = None
        for _ in range(120):
            time.sleep(0.5)
            new = [f for f in os.listdir(out_dir) if f not in before]
            if any(f.endswith(".part") for f in new):
                continue
            pages = [f for f in new if f.endswith(".html")]
            if pages:
                saved = pages[0]
                break
        if saved is None:
            return False
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(profile, ignore_errors=True)

    # saved is set above the try, and the only path that leaves it None
    # returns before here.
    # noinspection PyUnboundLocalVariable
    stem = os.path.splitext(saved)[0]
    tidy(os.path.join(out_dir, saved), os.path.join(out_dir, stem + "_files"), out_dir)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("url")
    ap.add_argument("out")
    args = ap.parse_args()

    os.makedirs(os.path.join(args.out, "a"), exist_ok=True)
    if save_by_browser(args.url, args.out):
        return
    print("the browser route did not work; fetching instead", file=sys.stderr)
    save_by_fetching(args.url, args.out)


if __name__ == "__main__":
    main()
