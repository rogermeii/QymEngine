"""Android 真机一键回归入口。"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ANDROID_DIR = ROOT / "android"
VERIFY_SCRIPT = ROOT / "scripts" / "verify_backends.py"
DEFAULT_OUTPUT_DIR = ROOT / "captures" / "verify" / "android_device"


def run(argv: list[str], cwd: Path) -> int:
    completed = subprocess.run(argv, cwd=str(cwd), check=False)
    return completed.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description="QymEngine Android 真机一键回归")
    parser.add_argument("--skip-build", action="store_true", help="跳过 assembleDebug，直接复用现有 APK")
    parser.add_argument("--skip-install", action="store_true", help="跳过 adb install -r")
    parser.add_argument(
        "--output-dir",
        default=str(DEFAULT_OUTPUT_DIR),
        help="结果输出目录，默认写入 captures/verify/android_device",
    )
    args = parser.parse_args()

    if not args.skip_build:
        print("[1/2] Building Android debug APK...")
        build_code = run([".\\gradlew.bat", ":app:assembleDebug"], ANDROID_DIR)
        if build_code != 0:
            return build_code

    print("[2/2] Running Android device regression...")
    verify_args = [
        sys.executable,
        str(VERIFY_SCRIPT),
        "--android-only",
        "--output-dir",
        str(Path(args.output_dir).resolve()),
    ]
    if args.skip_install:
        verify_args.append("--skip-android-install")
    return run(verify_args, ROOT)


if __name__ == "__main__":
    raise SystemExit(main())
