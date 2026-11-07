#!/usr/bin/env python3
"""Packs the Steam Frame download for FrameDrop: a zip with the release's own tar.gz and the script Steam runs when
you press Play, plus the manifest the Install with FrameDrop button points at.

Usage: make-framedrop.py <version> <folder with UTMTFTM-<version>-linux-arm64.tar.gz> [repo]
Writes UTMTFTM-<version>-framedrop.zip and utmtftm.framedrop.json into the same folder.
"""
import hashlib
import json
import os
import sys
import zipfile

version, folder = sys.argv[1], sys.argv[2]
repo = sys.argv[3] if len(sys.argv) > 3 else "RxVreality/UTMTFTM"
here = os.path.dirname(os.path.abspath(__file__))
tar = f"UTMTFTM-{version}-linux-arm64.tar.gz"
zip_name = f"UTMTFTM-{version}-framedrop.zip"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def add(z, arcname, data, mode):
    info = zipfile.ZipInfo(arcname, date_time=(2026, 1, 1, 0, 0, 0))  # fixed date, so the same build gives the same zip
    info.external_attr = (0o100000 | mode) << 16
    info.create_system = 3  # Unix, so tools that keep permissions keep the exec bit
    info.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(info, data)


with open(os.path.join(folder, tar), "rb") as f:
    tar_bytes = f.read()
with open(os.path.join(here, "install-utmtftm.sh"), "rb") as f:
    script = f.read()

zip_path = os.path.join(folder, zip_name)
with zipfile.ZipFile(zip_path, "w") as z:
    add(z, "UTMTFTM/install-utmtftm.sh", script, 0o755)
    add(z, f"UTMTFTM/{tar}", tar_bytes, 0o644)
    add(z, f"UTMTFTM/{tar}.sha256", f"{hashlib.sha256(tar_bytes).hexdigest()}  {tar}\n".encode(), 0o644)

manifest = {
    "schema": "framedrop.install/v1",
    "name": "UTMTFTM",
    "version": version,
    "files": [{
        "url": f"https://github.com/{repo}/releases/download/v{version}/{zip_name}",
        "sha256": sha256(zip_path),
    }],
}
with open(os.path.join(folder, "utmtftm.framedrop.json"), "w", newline="\n") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
with open(zip_path + ".sha256", "w", newline="\n") as f:
    f.write(f"{manifest['files'][0]['sha256']}  {zip_name}\n")
print(f"{zip_name} {os.path.getsize(zip_path)} bytes, sha256 {manifest['files'][0]['sha256']}")
