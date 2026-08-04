#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "source": ([sys.executable, str(ROOT / "tests" / "validate_sources.py")], 30.0),
    "cpu": (["make", "test-cpu"], 180.0),
    "cpu-spec": (["make", "test-cpu-spec"], 180.0),
    "gte": (["make", "test-gte"], 300.0),
    "cheats": (["make", "test-cheats"], 180.0),
    "gpu": (["make", "test-gpu"], 120.0),
    "texrep": (["make", "test-texrep"], 120.0),
    "gpu-profile": (["make", "test-gpu-profile"], 60.0),
    "raster-select": (["make", "test-raster-select"], 60.0),
    "present-dst": (["make", "test-present-dst"], 60.0),
    "audio-queue": (["make", "test-audio-queue"], 60.0),
    "mdec-bounds": (["make", "test-mdec-bounds"], 180.0),
    "spu-width": (["make", "test-spu-width"], 180.0),
    "mcard-diverge": (["make", "test-mcard-diverge"], 180.0),
    "cdrom-getlocp": (["make", "test-cdrom-getlocp"], 180.0),
    "chd": (["make", "test-chd"], 180.0),
    "disc-serial": (["make", "test-disc-serial"], 180.0),
    "zip": (["make", "test-zip"], 180.0),
    "web": ([sys.executable, str(ROOT / "tests" / "validate_web.py")], 30.0),
    "sdl-lifecycle": (["make", "test-sdl-lifecycle"], 60.0),
    "sdl-runtime": (["make", "test-sdl-runtime"], 60.0),
}


def resolve_make(explicit: str | None) -> list[str]:
    configured = explicit or os.environ.get("MAKE")
    if configured:
        command = shlex.split(configured)
        if not command:
            raise ValueError("MAKE resolved to an empty command")
        executable = shutil.which(command[0])
        if not executable:
            raise ValueError(f"make tool not found: {command[0]}")
        return [executable, *command[1:]]

    # On macOS /usr/bin/make may dispatch through the active Xcode developer directory.
    # Prefer an independently installed GNU Make when it is available.
    candidates = ["gmake", "make"] if platform.system() == "Darwin" else ["make", "gmake"]
    for candidate in candidates:
        executable = shutil.which(candidate)
        if executable:
            return [executable]
    raise ValueError("neither GNU make nor make was found")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run deterministic ARMSX validation cases.")
    parser.add_argument("--case", action="append", choices=tuple(CASES), dest="selected")
    parser.add_argument("--timeout-scale", type=float, default=1.0)
    parser.add_argument("--make-tool", help="make command (defaults to $MAKE, then gmake on macOS)")
    args = parser.parse_args()
    if args.timeout_scale <= 0:
        parser.error("--timeout-scale must be greater than zero")

    selected = args.selected or list(CASES)
    try:
        make_command = resolve_make(args.make_tool)
    except ValueError as error:
        parser.error(str(error))
    failures: list[str] = []
    started = time.monotonic()

    for name in selected:
        configured_command, timeout = CASES[name]
        command = ([*make_command, *configured_command[1:]]
                   if configured_command[0] == "make" else configured_command)
        print(f"ARMSX_VALIDATION begin case={name}", flush=True)
        case_started = time.monotonic()
        try:
            result = subprocess.run(
                command,
                cwd=ROOT,
                check=False,
                timeout=timeout * args.timeout_scale,
            )
        except subprocess.TimeoutExpired:
            failures.append(f"{name}:timeout")
            print(f"ARMSX_VALIDATION failed case={name} reason=timeout", file=sys.stderr, flush=True)
            continue

        elapsed = time.monotonic() - case_started
        if result.returncode:
            failures.append(f"{name}:exit-{result.returncode}")
            print(
                f"ARMSX_VALIDATION failed case={name} exit={result.returncode} elapsed={elapsed:.3f}s",
                file=sys.stderr,
                flush=True,
            )
        else:
            print(f"ARMSX_VALIDATION passed case={name} elapsed={elapsed:.3f}s", flush=True)

    elapsed = time.monotonic() - started
    if failures:
        print(f"ARMSX_VALIDATION failures={','.join(failures)} elapsed={elapsed:.3f}s", file=sys.stderr)
        return 1
    print(f"ARMSX_VALIDATION all selected cases passed elapsed={elapsed:.3f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
