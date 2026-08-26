"""
Locations of the vendored Microsoft material, resolved in one place.

Set DWRITECORE_THIRD_PARTY to point the tools at a copy held elsewhere.
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

THIRD_PARTY = os.environ.get(
    "DWRITECORE_THIRD_PARTY", os.path.join(ROOT, "third_party"))

# The six DirectWrite headers that are mirrored into include/.
DWRITE_SDK = os.path.join(THIRD_PARTY, "windows-app-sdk")

# The Windows SDK subset the DirectWrite headers depend on (shared/ and um/).
WINDOWS_SDK = os.path.join(THIRD_PARTY, "windows-sdk")

# Binaries taken from the Office Android APK.
OFFICE = os.path.join(THIRD_PARTY, "office-android")
BINARY = os.path.join(OFFICE, "libdwritecore.so")

# Generated output, all of it reproducible from the above.
INCLUDE = os.path.join(ROOT, "include")
SRC = os.path.join(ROOT, "src")
TESTS = os.path.join(ROOT, "tests")
BIONIC_COMPAT = os.path.join(ROOT, "bionic-compat")
DOCS = os.path.join(ROOT, "docs")
