#!/usr/bin/env python3
"""Check app-builder.json against the CardputerZero store's metadata rules (CP0_dev.md): required fields,
Debian package name, version equal to the CMake project version, share code, categories, the seven
permission flags, a square icon of 128-512 px and 320x170 screenshots. Exit status 1 lists every problem.

Usage: tools/validate_app_builder.py [repo_dir]
"""
import json
import re
import struct
import sys
from pathlib import Path

CATEGORIES = {"AI", "Creative & Office", "Development", "Education", "Emulators", "Games", "Hardware & IoT",
              "Lifestyle", "Media", "Other", "Radio & Comms", "Security", "System Tools"}
PERMISSIONS = {"camera", "microphone", "imu", "network", "additional_hardware", "background_service",
               "external_display"}


def png_size(path):
    """(width, height) from the IHDR chunk, or None when the file is not a PNG."""
    data = path.read_bytes()[:24]
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", data[16:24])


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    errors = []
    app = json.loads((root / "app-builder.json").read_text())
    store = app.get("store", {})

    for key in ("app_name", "package_name", "version"):
        if not app.get(key):
            errors.append(f"{key} is missing")
    if not re.fullmatch(r"[a-z0-9][a-z0-9+.-]+", app.get("package_name", "")):
        errors.append("package_name must be a Debian package name: a-z 0-9 . + -")
    cmake = re.search(r"project\(\s*\S+\s+VERSION\s+([0-9.]+)", (root / "CMakeLists.txt").read_text())
    if not cmake or cmake.group(1) != app.get("version"):
        errors.append(f"version {app.get('version')} differs from CMakeLists.txt ({cmake and cmake.group(1)})")

    for key in ("summary", "license", "share_code", "icon"):
        if not store.get(key):
            errors.append(f"store.{key} is missing")
    if not re.fullmatch(r"[A-Z0-9]{4}", store.get("share_code", "")):
        errors.append("store.share_code must be exactly 4 characters A-Z / 0-9")
    cats = store.get("categories", [])
    if not 1 <= len(cats) <= 2 or not set(cats) <= CATEGORIES:
        errors.append(f"store.categories must be 1-2 of {sorted(CATEGORIES)}")
    perms = store.get("permissions", {})
    if set(perms) != PERMISSIONS or not all(isinstance(v, bool) for v in perms.values()):
        errors.append(f"store.permissions must set all of {sorted(PERMISSIONS)} to true or false")
    if not store.get("author", {}).get("display_name"):
        errors.append("store.author.display_name is missing")

    if store.get("icon"):
        size = png_size(root / store["icon"]) if (root / store["icon"]).is_file() else None
        if not size or size[0] != size[1] or not 128 <= size[0] <= 512:
            errors.append(f"store.icon {store['icon']}: square PNG 128-512 px needed, got {size}")
    shots = store.get("screenshots", [])
    if not shots:
        errors.append("store.screenshots: at least one is required")
    for shot in shots:
        size = png_size(root / shot) if (root / shot).is_file() else None
        if size != (320, 170):
            errors.append(f"screenshot {shot}: PNG of exactly 320x170 needed, got {size}")

    for e in errors:
        print(f"app-builder.json: {e}")
    if not errors:
        print(f"app-builder.json: {app['package_name']} {app['version']} OK ({len(shots)} screenshots)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
