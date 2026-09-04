#!/usr/bin/env python3
"""Download and extract a CEF minimal binary distribution."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform as host_platform
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path, PurePosixPath

INDEX_URL = "https://cef-builds.spotifycdn.com/index.json"
DOWNLOAD_BASE_URL = "https://cef-builds.spotifycdn.com/"
DEFAULT_DESTINATION = Path(__file__).resolve().parents[1] / "third_party" / "cef"
INSTALL_MARKER = ".trail-browser-cef"


def detect_platform() -> str:
    system = host_platform.system()
    machine = host_platform.machine().lower()
    is_64_bit = sys.maxsize > 2**32

    if system == "Windows":
        if machine in {"arm64", "aarch64"}:
            return "windowsarm64"
        return "windows64" if is_64_bit else "windows32"
    if system == "Darwin":
        return "macosarm64" if machine in {"arm64", "aarch64"} else "macosx64"
    if system == "Linux":
        if machine in {"arm64", "aarch64"}:
            return "linuxarm64"
        if machine.startswith(("arm", "aarch")):
            return "linuxarm"
        if is_64_bit:
            return "linux64"
        raise RuntimeError("CEF no longer publishes Linux x86 32-bit builds")
    raise RuntimeError(f"unsupported operating system: {system}")


def chromium_version_key(version: dict[str, object]) -> tuple[int, ...]:
    return tuple(int(component) for component in str(version["chromium_version"]).split("."))


def find_release(
    releases: list[object], channel: str, version: str | None
) -> dict[str, object]:
    candidates = [
        item
        for item in releases
        if isinstance(item, dict)
        and (version is not None or item.get("channel") == channel)
        and (version is None or item.get("cef_version") == version)
    ]
    if not candidates:
        requested = version or f"latest {channel}"
        raise RuntimeError(f"no {requested} CEF release found")

    if version is not None:
        # The index may list the same version under multiple channels. Exact
        # version requests are intentionally independent of --channel.
        return candidates[0]
    return max(candidates, key=chromium_version_key)


def select_download(
    index: dict[str, object], cef_platform: str, channel: str, version: str | None
) -> tuple[dict[str, object], dict[str, object]]:
    platform_entry = index.get(cef_platform)
    if not isinstance(platform_entry, dict):
        raise RuntimeError(f"platform {cef_platform!r} is missing from the CEF index")

    releases = platform_entry.get("versions")
    if not isinstance(releases, list):
        raise RuntimeError(f"CEF index contains no releases for {cef_platform}")

    try:
        release = find_release(releases, channel, version)
    except RuntimeError as error:
        raise RuntimeError(f"{error} for {cef_platform}") from error
    files = release.get("files")
    if not isinstance(files, list):
        raise RuntimeError("selected CEF release has no downloadable files")
    archive = next(
        (item for item in files if isinstance(item, dict) and item.get("type") == "minimal"),
        None,
    )
    if archive is None:
        raise RuntimeError("selected CEF release has no minimal distribution")
    return release, archive


def download(url: str, destination: Path, expected_size: int) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "TrailBrowser/fetch-cef"})
    with urllib.request.urlopen(request, timeout=60) as response, destination.open("wb") as output:
        downloaded = 0
        while block := response.read(1024 * 1024):
            output.write(block)
            downloaded += len(block)
            if expected_size:
                percent = min(100, downloaded * 100 // expected_size)
                print(
                    f"\rDownloading: {percent:3d}% "
                    f"({downloaded / 1024 / 1024:.1f} MiB)",
                    end="",
                    flush=True,
                )
    print()


def sha1(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def safe_extract(archive: Path, destination: Path) -> Path:
    destination_resolved = destination.resolve()
    with tarfile.open(archive, mode="r:bz2") as bundle:
        members = bundle.getmembers()
        roots: set[str] = set()
        link_paths = {
            PurePosixPath(member.name)
            for member in members
            if member.issym() or member.islnk()
        }
        for member in members:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or "\\" in member.name:
                raise RuntimeError(f"unsafe path in CEF archive: {member.name}")
            if member.isdev() or member.isfifo():
                raise RuntimeError(f"unsupported special file in CEF archive: {member.name}")
            if any(parent in link_paths for parent in path.parents):
                raise RuntimeError(f"path traverses an archive link: {member.name}")
            if path.parts:
                roots.add(path.parts[0])
            if member.issym() or member.islnk():
                if "\\" in member.linkname:
                    raise RuntimeError(f"unsafe link in CEF archive: {member.name}")
                link_base = path.parent if member.issym() else PurePosixPath()
                resolved_link = (destination_resolved / link_base / member.linkname).resolve()
                if os.path.commonpath((destination_resolved, resolved_link)) != str(destination_resolved):
                    raise RuntimeError(f"unsafe link in CEF archive: {member.name}")
        if len(roots) != 1:
            raise RuntimeError("CEF archive must contain exactly one top-level directory")
        bundle.extractall(destination)
    return destination / roots.pop()


def validate_replace_target(destination: Path) -> None:
    protected = {
        Path(destination.anchor),
        Path.home().resolve(),
        Path.cwd().resolve(),
        Path(__file__).resolve().parents[1],
    }
    if destination in protected:
        raise RuntimeError(f"refusing to replace protected directory: {destination}")
    if any(destination.iterdir()) and not (
        (destination / INSTALL_MARKER).is_file()
        or (destination / "cmake" / "FindCEF.cmake").is_file()
    ):
        raise RuntimeError(
            f"refusing to replace non-CEF directory: {destination}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", help="CEF platform (auto-detected by default)")
    parser.add_argument("--channel", default="stable", choices=("stable", "beta"))
    parser.add_argument("--version", help="exact CEF version; defaults to latest channel version")
    parser.add_argument(
        "--destination",
        type=Path,
        default=DEFAULT_DESTINATION,
        help=f"extraction directory (default: {DEFAULT_DESTINATION})",
    )
    parser.add_argument("--force", action="store_true", help="replace an existing destination")
    parser.add_argument("--print-url", action="store_true", help="print selection without downloading")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cef_platform = args.platform or detect_platform()
    print(f"Reading CEF build index for {cef_platform}...")
    with urllib.request.urlopen(INDEX_URL, timeout=60) as response:
        index = json.load(response)

    release, archive = select_download(index, cef_platform, args.channel, args.version)
    archive_name = str(archive["name"])
    archive_url = urllib.request.urljoin(DOWNLOAD_BASE_URL, archive_name)
    expected_sha1 = str(archive["sha1"])
    expected_size = int(archive.get("size", 0))
    print(f"CEF:      {release['cef_version']}")
    print(f"Chromium: {release['chromium_version']}")
    print(f"Archive:  {archive_url}")
    if args.print_url:
        return 0

    destination = args.destination.expanduser().resolve()
    if destination.exists():
        if not args.force:
            raise RuntimeError(f"destination already exists: {destination} (use --force to replace it)")
        validate_replace_target(destination)

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="trail-cef-", dir=destination.parent) as temporary:
        temporary_path = Path(temporary)
        archive_path = temporary_path / archive_name
        download(archive_url, archive_path, expected_size)
        actual_sha1 = sha1(archive_path)
        if actual_sha1.lower() != expected_sha1.lower():
            raise RuntimeError(f"SHA-1 mismatch: expected {expected_sha1}, got {actual_sha1}")

        print("Extracting...")
        extracted_root = safe_extract(archive_path, temporary_path / "extracted")
        if destination.exists():
            shutil.rmtree(destination)
        shutil.move(str(extracted_root), destination)
        (destination / INSTALL_MARKER).write_text(
            f"{release['cef_version']}\n", encoding="utf-8"
        )

    print(f"CEF_ROOT={destination}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
