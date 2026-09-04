#!/usr/bin/env python3
"""Configure a local Trail Browser CMake build."""

from __future__ import annotations

import argparse
import os
import subprocess
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CEF_ROOT = PROJECT_ROOT / "third_party" / "cef"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cef-root", type=Path, default=None)
    parser.add_argument("--build-dir", type=Path, default=PROJECT_ROOT / "build")
    parser.add_argument("--build-type", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--generator", help="CMake generator, for example Ninja")
    args, cmake_args = parser.parse_known_args()

    cef_root = args.cef_root or (Path(os.environ["CEF_ROOT"]) if "CEF_ROOT" in os.environ else DEFAULT_CEF_ROOT)
    cef_root = cef_root.expanduser().resolve()
    if not (cef_root / "cmake" / "FindCEF.cmake").is_file():
        parser.error(f"CEF distribution not found at {cef_root}; run scripts/fetch_cef.py first")

    command = [
        "cmake",
        "-S",
        str(PROJECT_ROOT),
        "-B",
        str(args.build_dir.expanduser().resolve()),
        f"-DCEF_ROOT={cef_root}",
        f"-DCMAKE_BUILD_TYPE={args.build_type}",
    ]
    if args.generator:
        command.extend(("-G", args.generator))
    command.extend(cmake_args)
    print("Running:", " ".join(command))
    return subprocess.call(command)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as error:
        raise SystemExit(f"error: unable to run CMake: {error}")
