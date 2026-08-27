#!/usr/bin/env python3
"""
Report the advance width of a string, per family and size, over DevTools.

    tools/testing/advance_probe_cdp.py <host> <port>

A rasterization comparison is only meaningful once the two sides agree on
where each glyph goes. Windows measures advances linearly - DirectWrite is
asked with DWRITE_MEASURING_MODE_NATURAL at the real text size - while a
Fontations scaler on Linux returns grid-fitted ones, and the difference is
far larger than the half-pixel a rounding disagreement could explain.
"""
import sys, json
import viewport_protocol as vp

JS = """
const out = [];
const probe = document.createElement('span');
probe.style.cssText='position:absolute;left:-9999px;white-space:pre';
document.body.appendChild(probe);
const text='The quick brown fox jumps over the lazy dog 0123456789';
for (const fam of ['Arial','Times New Roman','Courier New','Segoe UI']) {
  for (const size of [11,12,13,14,16,18,20,24,32]) {
    probe.style.font = size+'px "'+fam+'"';
    probe.textContent = text;
    out.push(fam+'|'+size+'|'+probe.getBoundingClientRect().width.toFixed(3));
  }
}
probe.remove();
return out.join('\\n');
"""
b = vp.CdpBrowser(sys.argv[1], int(sys.argv[2]))
try:
    print(b.script(JS))
finally:
    b.close()
