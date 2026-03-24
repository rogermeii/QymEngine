"""QymEngine 多后端回归脚本。

顺序验证：
1. Windows: Vulkan / D3D12 / D3D11 / OpenGL / GLES
2. Android: Vulkan / GLES

Windows 通过编辑器自动化接口收集 draw stats、scene info 和截图。
Android 通过 adb 启动应用、读取日志并抓取系统截图。
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional

try:
    from PIL import Image
except Exception:  # pragma: no cover - Pillow 是可选依赖
    Image = None


ROOT = Path(__file__).resolve().parents[1]
EDITOR_EXE = ROOT / "build3" / "editor" / "Debug" / "QymEditor.exe"
ANDROID_APK = ROOT / "android" / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk"
COMMAND_FILE = ROOT / "captures" / "command.json"
RESULT_FILE = ROOT / "captures" / "command_result.json"
VERIFY_DIR = ROOT / "captures" / "verify" / "backend_regression"
RESULT_JSON = VERIFY_DIR / "results.json"
RESULT_TXT = VERIFY_DIR / "summary.txt"

ANDROID_PACKAGE = "com.qymengine.app"
ANDROID_ACTIVITY = "com.qymengine.app/.QymActivity"

EXPECTED_NODES = {"Ground", "Center Cube", "Sphere", "Sun Light"}

WINDOWS_BACKENDS = [
    {"name": "windows_vulkan", "label": "Windows Vulkan", "args": []},
    {"name": "windows_d3d12", "label": "Windows D3D12", "args": ["--d3d12"]},
    {"name": "windows_d3d11", "label": "Windows D3D11", "args": ["--d3d11"]},
    {"name": "windows_opengl", "label": "Windows OpenGL", "args": ["--opengl"]},
    {"name": "windows_gles", "label": "Windows GLES", "args": ["--gles"]},
]

ANDROID_BACKENDS = [
    {"name": "android_vulkan", "label": "Android Vulkan", "args": []},
    {"name": "android_gles", "label": "Android GLES", "args": ["--gles"]},
]


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def run_command(
    argv: list[str],
    cwd: Optional[Path] = None,
    timeout: Optional[float] = None,
    check: bool = True,
    capture_output: bool = True,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv,
        cwd=str(cwd) if cwd else None,
        timeout=timeout,
        check=check,
        capture_output=capture_output,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def kill_editor_processes() -> None:
    subprocess.run(
        ["taskkill", "/F", "/IM", "QymEditor.exe"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def send_editor_command(command: str, params: Optional[dict[str, Any]] = None, timeout: float = 6.0) -> dict[str, Any]:
    params = params or {}
    RESULT_FILE.unlink(missing_ok=True)
    ensure_dir(COMMAND_FILE.parent)
    with COMMAND_FILE.open("w", encoding="utf-8") as f:
        json.dump({"command": command, "params": params}, f)

    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.1)
        if RESULT_FILE.exists() and not COMMAND_FILE.exists():
            try:
                return json.loads(RESULT_FILE.read_text(encoding="utf-8"))
            except Exception as exc:
                return {"status": "error", "message": f"invalid json: {exc}"}

    return {"status": "timeout", "message": f"{command} timed out"}


def request_editor_command(
    command: str,
    params: Optional[dict[str, Any]] = None,
    timeout: float = 6.0,
    retries: int = 1,
    retry_delay: float = 0.5,
) -> dict[str, Any]:
    last_result: dict[str, Any] = {"status": "timeout", "message": f"{command} timed out"}
    for attempt in range(retries):
        last_result = send_editor_command(command, params=params, timeout=timeout)
        if last_result.get("status") == "ok":
            return last_result
        if attempt + 1 < retries:
            time.sleep(retry_delay)
    return last_result


def wait_for_editor_ready(timeout: float = 25.0) -> dict[str, Any]:
    deadline = time.time() + timeout
    last_result: dict[str, Any] = {"status": "timeout", "message": "editor not ready"}
    while time.time() < deadline:
        last_result = send_editor_command("get_draw_stats", timeout=3.0)
        if last_result.get("status") == "ok" and last_result.get("draw_calls", 0) > 0:
            return last_result
        time.sleep(0.5)
    return last_result


def analyze_png(path: Path) -> dict[str, Any]:
    info: dict[str, Any] = {
        "path": str(path),
        "exists": path.exists(),
        "size_bytes": path.stat().st_size if path.exists() else 0,
    }
    if not path.exists():
        return info

    if Image is None:
        info["analysis"] = "skipped_no_pillow"
        return info

    try:
        with Image.open(path) as img:
            rgba = img.convert("RGBA")
            width, height = rgba.size
            colors = rgba.getcolors(maxcolors=5_000_000)
            info["width"] = width
            info["height"] = height
            info["unique_colors"] = None if colors is None else len(colors)
            info["uniform"] = colors is not None and len(colors) == 1
            info["center_pixel"] = rgba.getpixel((width // 2, height // 2))
            if colors:
                colors_sorted = sorted(colors, reverse=True)
                info["top_color_ratio"] = colors_sorted[0][0] / float(width * height)
                info["top2_color_ratio"] = sum(count for count, _ in colors_sorted[:2]) / float(width * height)

            # Android 全屏截图会包含 UI，额外分析中间的大块 Scene View 区域。
            crop = rgba.crop((int(width * 0.22), int(height * 0.08), int(width * 0.80), int(height * 0.75)))
            crop_colors = crop.getcolors(maxcolors=5_000_000)
            info["scene_unique_colors"] = None if crop_colors is None else len(crop_colors)
            if crop_colors:
                crop_sorted = sorted(crop_colors, reverse=True)
                crop_pixels = float(crop.width * crop.height)
                info["scene_top_color_ratio"] = crop_sorted[0][0] / crop_pixels
                info["scene_top2_color_ratio"] = sum(count for count, _ in crop_sorted[:2]) / crop_pixels
    except Exception as exc:
        info["analysis_error"] = str(exc)
    return info


def write_summary(results: list[dict[str, Any]]) -> None:
    ensure_dir(VERIFY_DIR)
    RESULT_JSON.write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8")

    lines = []
    passed = 0
    failed = 0
    skipped = 0
    for result in results:
        status = result["status"]
        if status == "passed":
            passed += 1
        elif status == "failed":
            failed += 1
        else:
            skipped += 1
        lines.append(f"[{status.upper()}] {result['label']}: {result.get('message', '')}")

    lines.append("")
    lines.append(f"Passed: {passed}")
    lines.append(f"Failed: {failed}")
    lines.append(f"Skipped: {skipped}")
    RESULT_TXT.write_text("\n".join(lines), encoding="utf-8")


def validate_windows_result(result: dict[str, Any]) -> tuple[bool, str]:
    stats = result.get("draw_stats", {})
    scene_info = result.get("scene_info", {})
    screenshot = result.get("screenshot", {})

    if stats.get("status") != "ok":
        return False, "draw stats unavailable"
    if stats.get("draw_calls", 0) <= 0:
        return False, f"draw_calls={stats.get('draw_calls', 0)}"
    if stats.get("triangles", 0) <= 0:
        return False, f"triangles={stats.get('triangles', 0)}"

    if scene_info.get("status") != "ok":
        return False, "scene info unavailable"
    node_names = {node.get("name", "") for node in scene_info.get("nodes", [])}
    if not EXPECTED_NODES.issubset(node_names):
        return False, f"missing nodes: {sorted(EXPECTED_NODES - node_names)}"

    if not screenshot.get("exists"):
        return False, "screenshot missing"
    if screenshot.get("size_bytes", 0) < 4096:
        return False, f"screenshot too small: {screenshot.get('size_bytes', 0)}"
    unique_colors = screenshot.get("unique_colors")
    if unique_colors is not None and unique_colors <= 1:
        return False, "uniform screenshot"

    return True, (
        f"draw_calls={stats.get('draw_calls')} triangles={stats.get('triangles')} "
        f"unique_colors={screenshot.get('unique_colors')}"
    )


def run_windows_backend(backend: dict[str, Any]) -> dict[str, Any]:
    ensure_dir(VERIFY_DIR)
    kill_editor_processes()
    COMMAND_FILE.unlink(missing_ok=True)
    RESULT_FILE.unlink(missing_ok=True)

    log_path = VERIFY_DIR / f"{backend['name']}.log"
    shot_path = VERIFY_DIR / f"{backend['name']}.png"
    shot_path.unlink(missing_ok=True)

    with log_path.open("w", encoding="utf-8", errors="replace") as log_file:
        proc = subprocess.Popen(
            [str(EDITOR_EXE), *backend["args"]],
            cwd=str(ROOT),
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

        result: dict[str, Any] = {
            "name": backend["name"],
            "label": backend["label"],
            "platform": "windows",
            "status": "failed",
            "log_path": str(log_path),
            "screenshot_path": str(shot_path),
        }

        try:
            stats = wait_for_editor_ready()
            result["draw_stats"] = stats
            if stats.get("status") != "ok" or stats.get("draw_calls", 0) <= 0:
                result["message"] = f"editor not ready: {stats}"
                return result

            scene_info = request_editor_command("get_scene_info", timeout=10.0, retries=3)
            result["scene_info"] = scene_info

            shot_result = request_editor_command(
                "screenshot",
                {"path": shot_path.as_posix()},
                timeout=20.0,
                retries=2,
            )
            result["screenshot_command"] = shot_result
            time.sleep(1.0)
            result["screenshot"] = analyze_png(shot_path)

            ok, message = validate_windows_result(result)
            result["status"] = "passed" if ok else "failed"
            result["message"] = message
            return result
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
            kill_editor_processes()
            COMMAND_FILE.unlink(missing_ok=True)
            RESULT_FILE.unlink(missing_ok=True)


def find_adb() -> Optional[str]:
    return shutil.which("adb")


def list_android_devices(adb: str) -> list[str]:
    result = run_command([adb, "devices"], cwd=ROOT)
    devices = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if not line or line.startswith("List of devices attached"):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices


def poll_android_log(adb: str, expected_text: str, timeout: float = 15.0) -> str:
    deadline = time.time() + timeout
    last_log = ""
    while time.time() < deadline:
        log_result = run_command([adb, "logcat", "-d"], cwd=ROOT)
        last_log = log_result.stdout
        if expected_text in last_log:
            return last_log
        time.sleep(0.5)
    return last_log


def analyze_android_result(result: dict[str, Any]) -> tuple[bool, str]:
    if result.get("pid", 0) <= 0:
        return False, "process not running"

    if not result.get("backend_confirmed", False):
        return False, "backend log not confirmed"

    screenshot = result.get("screenshot", {})
    if not screenshot.get("exists"):
        return False, "screenshot missing"
    if screenshot.get("size_bytes", 0) < 16_384:
        return False, f"screenshot too small: {screenshot.get('size_bytes', 0)}"
    scene_ratio = screenshot.get("scene_top2_color_ratio")
    if scene_ratio is not None and scene_ratio >= 0.85:
        return False, f"scene view looks blank: top2_ratio={scene_ratio:.3f}"

    return True, (
        f"pid={result.get('pid')} scene_top2_ratio={screenshot.get('scene_top2_color_ratio')} "
        f"size={screenshot.get('size_bytes')}"
    )


def run_android_backend(backend: dict[str, Any], adb: str, install_apk: bool) -> dict[str, Any]:
    ensure_dir(VERIFY_DIR)

    result: dict[str, Any] = {
        "name": backend["name"],
        "label": backend["label"],
        "platform": "android",
        "status": "failed",
    }

    shot_path = VERIFY_DIR / f"{backend['name']}.png"
    log_path = VERIFY_DIR / f"{backend['name']}.log"
    shot_path.unlink(missing_ok=True)
    log_path.unlink(missing_ok=True)
    result["screenshot_path"] = str(shot_path)
    result["log_path"] = str(log_path)

    run_command([adb, "shell", "am", "force-stop", ANDROID_PACKAGE], cwd=ROOT, check=False)
    run_command([adb, "logcat", "-c"], cwd=ROOT, check=False)

    if install_apk and ANDROID_APK.exists():
        install_result = run_command([adb, "install", "-r", str(ANDROID_APK)], cwd=ROOT, check=False)
        result["install_stdout"] = install_result.stdout
        result["install_stderr"] = install_result.stderr

    start_cmd = [adb, "shell", "am", "start", "-W", "-n", ANDROID_ACTIVITY]
    if backend["args"]:
        start_cmd.extend(["--es", "args", " ".join(backend["args"])])
    start_result = run_command(start_cmd, cwd=ROOT, check=False)
    result["start_stdout"] = start_result.stdout
    result["start_stderr"] = start_result.stderr

    time.sleep(2.0)

    pid_result = run_command([adb, "shell", "pidof", ANDROID_PACKAGE], cwd=ROOT, check=False)
    try:
        result["pid"] = int(pid_result.stdout.strip())
    except Exception:
        result["pid"] = 0

    expected_backend_line = "Using GLES backend" if backend["args"] else "Using Vulkan backend"
    log_text = poll_android_log(adb, expected_backend_line, timeout=12.0)
    log_path.write_text(log_text, encoding="utf-8", errors="replace")
    result["backend_confirmed"] = expected_backend_line in log_text

    # 给移动端多留几帧，把 Scene View 真正渲染出来再截图。
    time.sleep(3.0)

    with shot_path.open("wb") as image_file:
        subprocess.run([adb, "exec-out", "screencap", "-p"], cwd=str(ROOT), stdout=image_file, check=False)
    result["screenshot"] = analyze_png(shot_path)

    ok, message = analyze_android_result(result)
    result["status"] = "passed" if ok else "failed"
    result["message"] = message
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="QymEngine 多后端回归脚本")
    parser.add_argument("--windows-only", action="store_true", help="只跑 Windows 后端")
    parser.add_argument("--android-only", action="store_true", help="只跑 Android 后端")
    parser.add_argument(
        "--skip-android-install",
        action="store_true",
        help="Android 测试前不执行 adb install -r",
    )
    args = parser.parse_args()

    ensure_dir(VERIFY_DIR)

    if args.windows_only and args.android_only:
        print("不能同时指定 --windows-only 和 --android-only", file=sys.stderr)
        return 2

    results: list[dict[str, Any]] = []

    if not args.android_only:
        if not EDITOR_EXE.exists():
            results.append({
                "name": "windows_setup",
                "label": "Windows Setup",
                "platform": "windows",
                "status": "failed",
                "message": f"editor missing: {EDITOR_EXE}",
            })
        else:
            for backend in WINDOWS_BACKENDS:
                print(f"[Windows] {backend['label']}")
                backend_result = run_windows_backend(backend)
                print(f"  -> {backend_result['status']}: {backend_result.get('message', '')}")
                results.append(backend_result)

    if not args.windows_only:
        adb = find_adb()
        if not adb:
            results.append({
                "name": "android_setup",
                "label": "Android Setup",
                "platform": "android",
                "status": "skipped",
                "message": "adb not found",
            })
        else:
            devices = list_android_devices(adb)
            if not devices:
                results.append({
                    "name": "android_setup",
                    "label": "Android Setup",
                    "platform": "android",
                    "status": "skipped",
                    "message": "no connected device",
                })
            else:
                install_apk = not args.skip_android_install
                for backend in ANDROID_BACKENDS:
                    print(f"[Android] {backend['label']}")
                    backend_result = run_android_backend(backend, adb, install_apk)
                    print(f"  -> {backend_result['status']}: {backend_result.get('message', '')}")
                    results.append(backend_result)

    write_summary(results)

    failed = [result for result in results if result["status"] == "failed"]
    print("")
    print(f"Results saved to: {RESULT_JSON}")
    print(f"Summary saved to: {RESULT_TXT}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
