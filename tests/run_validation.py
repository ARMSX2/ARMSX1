#!/usr/bin/env python3

import argparse
from pathlib import Path
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "source": ([sys.executable, str(ROOT / "tests" / "validate_sources.py")], 30.0),
    "cpu": (["make", "test-cpu"], 180.0),
    "gpu": (["make", "test-gpu"], 120.0),
    "chd": (["make", "test-chd"], 180.0),
    "zip": (["make", "test-zip"], 180.0),
    "web": ([sys.executable, str(ROOT / "tests" / "validate_web.py")], 30.0),
}


def main() -> int:
    parser = argparse.ArgumentParser(description="Run deterministic ARMSX validation cases.")
    parser.add_argument("--case", action="append", choices=tuple(CASES), dest="selected")
    parser.add_argument("--timeout-scale", type=float, default=1.0)
    args = parser.parse_args()
    if args.timeout_scale <= 0:
        parser.error("--timeout-scale must be greater than zero")

    selected = args.selected or list(CASES)
    failures: list[str] = []
    started = time.monotonic()

    for name in selected:
        command, timeout = CASES[name]
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
