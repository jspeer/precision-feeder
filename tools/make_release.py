#!/usr/bin/env python3
"""Turn the prebuilt images (tools/build_bins.sh) into a release: versioned image names, the update check's
manifest, the install page's manifests and the release notes (DESIGN.md section 9.5).

    python3 tools/make_release.py <images dir> <site dir> <version>

<images dir> holds precision-feeder-<variant>.factory.bin and .ota.bin from tools/build_bins.sh. Writes:
  <site dir>/precision-feeder-<variant>-<version>.factory.bin and .ota.bin   (the same images, versioned names)
  <site dir>/manifest.json            the web app's update check (Firmware page): version, release notes link,
                                      and each variant's image names, sizes and SHA-256
  <site dir>/install-<variant>.json   ESP Web Tools' manifest for the install page (factory image at address 0)
  <site dir>/index.html               the install page (install/index.html)
  <site dir>/release-notes.md         this version's section of CHANGELOG.md, for the GitHub release
The version must be fw_version in packages/core.yaml, so a tag cannot publish images that report another version.

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPO = "https://github.com/jspeer/precision-feeder"
VARIANTS = ["full", "no-ble"]
TITLES = {"full": "Precision Feeder (with Bluetooth)", "no-ble": "Precision Feeder (no Bluetooth)"}


def fw_version() -> str:
    m = re.search(r'^\s*fw_version:\s*"([^"]+)"', (ROOT / "packages" / "core.yaml").read_text(), re.M)
    return m.group(1) if m else ""


def release_notes(version: str, changelog: str) -> str:
    """The CHANGELOG.md section whose heading starts with this version, without the heading, followed by any
    sections right after it whose heading says "not released" (their changes reach users in this release), each
    under its own heading."""
    sections = re.findall(r"^## ([^\n]*)\n(.*?)(?=^## |\Z)", changelog, re.M | re.S)
    for i, (heading, body) in enumerate(sections):
        if re.match(rf"{re.escape(version)}(?=[:\s])", heading):
            notes = body.strip() + "\n"
            for later, later_body in sections[i + 1:]:
                if "not released" not in later:
                    break
                notes += f"\n## {later}\n\n{later_body.strip()}\n"
            return notes
    return ""


def absolute_links(notes: str, version: str) -> str:
    """CHANGELOG.md links are relative to the repository; on a release page they must point at the tagged files."""
    return re.sub(r"\]\((?!https?://|#)([^)]+)\)", rf"]({REPO}/blob/v{version}/\1)", notes)


def image(path: pathlib.Path) -> dict:
    data = path.read_bytes()
    return {"name": path.name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def make(images: pathlib.Path, site: pathlib.Path, version: str) -> dict:
    site.mkdir(parents=True, exist_ok=True)
    manifest = {
        "version": version,
        "notes": f"{REPO}/releases/tag/v{version}",
        "releases": f"{REPO}/releases",
        "variants": {},
    }
    for variant in VARIANTS:
        entry = {}
        for kind in ("factory", "ota"):
            src = images / f"precision-feeder-{variant}.{kind}.bin"
            dst = site / f"precision-feeder-{variant}-{version}.{kind}.bin"
            shutil.copyfile(src, dst)
            entry[kind] = image(dst)
        manifest["variants"][variant] = entry
        install = {
            "name": TITLES[variant],
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": entry["factory"]["name"], "offset": 0}]}],
        }
        (site / f"install-{variant}.json").write_text(json.dumps(install, indent=2) + "\n")
    (site / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    shutil.copyfile(ROOT / "install" / "index.html", site / "index.html")
    notes = release_notes(version, (ROOT / "CHANGELOG.md").read_text())
    (site / "release-notes.md").write_text(absolute_links(notes, version))
    return manifest


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__.split("\n\n")[1])
        return 2
    images, site, version = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3].removeprefix("v")
    if version != fw_version():
        print(f"version {version} is not fw_version {fw_version()} in packages/core.yaml")
        return 1
    manifest = make(images, site, version)
    if not (site / "release-notes.md").read_text().strip():
        print(f"warning: no '## {version}' section in CHANGELOG.md")
    for variant, entry in manifest["variants"].items():
        print(f"{variant}: {entry['ota']['size']} bytes, sha256 {entry['ota']['sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
