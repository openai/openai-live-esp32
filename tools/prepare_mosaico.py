#!/usr/bin/env python3
"""Prepare the pinned Mosaico BSP, USB console and private configuration."""

import argparse
import hashlib
import io
import os
import subprocess
import tarfile
import urllib.request
from pathlib import Path

from prepare_peer import prepare
from prepare_voice import load_wifi, write_secrets

IDF_COMMIT = "7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe"
BSP_COMMIT = "5f88b91e1be055d1d3674364123b2779a1758377"
BSP_SHA256 = "cf4b1883515f1ec34c6209b5945fbc688978c9fcf3805105463435d4d93b312e"
USB_COMMIT = "2654a4a9f76f5d7ea108e4dff7ade7b1df4c1c5c"
USB_FILES = {
    "usb_console.c": "4ee7e603c87c172759e61835ba55778cca2ee69476b0aa2fd8cab1fa079ef09b",
    "usb_console.h": "274bdacc0dff7152bb6076e5b65a4aebd8ab7ac1cfb1c2c1ea90bb1ba37fddb7",
    "CMakeLists.txt": "86eafc486ff323255208d6b0d53457ff52548a4835a56224b6f0d334a294fe92",
    "Kconfig": "96d2b3d4c54832f046a7cb2054716d4eebb0805bbc3d67e417a084a7af36bba5",
    "idf_component.yml": "9d83ff6d522e0c7f14b9b7d692278a5c19e06ff60d1815013126dd0a0b595b74",
}


def validate_idf():
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise ValueError("Activate the dedicated Mosaico ESP-IDF checkout first")
    try:
        commit = subprocess.check_output(
            ["git", "-C", idf_path, "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.PIPE,
        ).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise ValueError("Mosaico requires the pinned ESP-IDF Git checkout") from error
    if commit != IDF_COMMIT:
        raise ValueError(
            f"Mosaico requires ESP-IDF {IDF_COMMIT}; keep the S3 checkout separate"
        )


def fetch(url, digest):
    with urllib.request.urlopen(url, timeout=90) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"Source checksum mismatch: {url}")
    return data


def dependencies(root):
    root.mkdir(parents=True, exist_ok=True)
    bsp = root / "esp-mosaico-bsp"
    if not bsp.exists():
        data = fetch(
            f"https://codeload.github.com/esp-mosaico/esp-mosaico-bsp/tar.gz/{BSP_COMMIT}",
            BSP_SHA256,
        )
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            archive.extractall(root, filter="data")
        (root / f"esp-mosaico-bsp-{BSP_COMMIT}").rename(bsp)
        (bsp / ".live-bsp-pin").write_text(BSP_COMMIT)
    elif (bsp / ".git").exists():
        # Development checkouts are allowed only at the reviewed source pin.
        commit = subprocess.check_output(
            ["git", "-C", str(bsp), "rev-parse", "HEAD"], text=True
        ).strip()
        if commit != BSP_COMMIT:
            raise ValueError("BSP checkout differs from the pinned revision")
    elif (
        not (bsp / ".live-bsp-pin").exists()
        or (bsp / ".live-bsp-pin").read_text() != BSP_COMMIT
    ):
        raise ValueError("Unrecognized BSP cache; use a fresh cache directory")
    usb = root / "usb_hs_console"
    usb.mkdir(exist_ok=True)
    for name, digest in USB_FILES.items():
        path = usb / name
        if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == digest:
            continue
        path.write_bytes(
            fetch(
                f"https://raw.githubusercontent.com/esp-mosaico/esp-mosaico-claw/{USB_COMMIT}"
                f"/boards/esp_mosaico/components/usb_hs_console/{name}",
                digest,
            )
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cache", type=Path, default=Path.home() / ".cache/openai-live-mosaico"
    )
    parser.add_argument(
        "--vendor-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "examples/mosaico/vendor",
        help="Ignored board sources; the default keeps dependency-lock paths portable",
    )
    parser.add_argument("--key-file", type=Path)
    parser.add_argument("--wifi-file", type=Path, required=True)
    args = parser.parse_args()
    validate_idf()
    key = (
        args.key_file.read_text().strip()
        if args.key_file
        else os.environ.get("OPENAI_API_KEY", "")
    )
    if not key:
        parser.error("Set OPENAI_API_KEY or supply --key-file")
    networks = load_wifi(args.wifi_file)
    if not networks:
        parser.error("At least one Wi-Fi profile is required")
    args.cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    args.cache.chmod(0o700)
    write_secrets(args.cache / "private", key, networks)
    dependencies(args.vendor_dir)
    prepare(args.cache / "vendor/peer", native_crypto=True)
    print(f"Mosaico dependencies: {args.vendor_dir}")
    print(f"Private configuration: {args.cache / 'private'}")


if __name__ == "__main__":
    main()
