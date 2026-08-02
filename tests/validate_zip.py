#!/usr/bin/env python3

from pathlib import Path
import subprocess
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "build" / "tests" / "zip-integration"


def main() -> int:
    if len(sys.argv) != 2:
        print("ZIP_VALIDATION failed reason=usage", file=sys.stderr)
        return 1

    FIXTURES.mkdir(parents=True, exist_ok=True)
    valid_zip = FIXTURES / "valid.zip"
    unsafe_zip = FIXTURES / "unsafe.zip"
    cue = (
        'FILE "disc.bin" BINARY\n'
        "  TRACK 01 MODE2/2352\n"
        "    INDEX 01 00:00:00\n"
    )

    with zipfile.ZipFile(valid_zip, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("nested/disc.bin", bytes(2352))
        archive.writestr("nested/disc.cue", cue)
    with zipfile.ZipFile(unsafe_zip, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("../escape.iso", bytes(2048))

    result = subprocess.run(
        [sys.argv[1], str(valid_zip), str(unsafe_zip)],
        cwd=ROOT,
        check=False,
    )
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
