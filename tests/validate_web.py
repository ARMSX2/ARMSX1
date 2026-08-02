#!/usr/bin/env python3

from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    node = shutil.which("node")
    cmake = shutil.which("cmake")
    if not node or not cmake:
        print("WEB_VALIDATION skipped: node and cmake are required")
        return 0

    subprocess.run([node, str(ROOT / "tests" / "web_file_access.test.js")], check=True, cwd=ROOT)
    with tempfile.TemporaryDirectory(prefix="armsx-web-test-") as directory:
        html = Path(directory) / "armsx.html"
        html.write_text(
            '<html><head></head><body><canvas id="canvas" tabindex=-1></canvas></body></html>',
            encoding="utf-8",
        )
        subprocess.run(
            [
                cmake,
                f"-DINPUT_FILE={html}",
                "-P",
                str(ROOT / "web" / "postprocess_web_html.cmake"),
            ],
            check=True,
            cwd=ROOT,
        )
        rendered = html.read_text(encoding="utf-8")
        if "ARMSXWebFiles" not in rendered or 'tabindex="0"' not in rendered:
            raise RuntimeError("web postprocessor did not inject the permission bridge")

    print("WEB_VALIDATION all cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
