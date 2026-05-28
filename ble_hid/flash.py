"""Windows 一键刷写 / 监视脚本（也兼容 macOS / Linux）。

用法（在 ble_hid/ 目录下运行）：
    python flash.py                  # 自动找端口 → 刷写 → 进入 monitor
    python flash.py --port COM5      # 指定端口
    python flash.py --no-monitor     # 只刷不进 monitor
    python flash.py --monitor-only   # 只 monitor（不刷写）
    python flash.py --erase          # 刷写前先 erase_flash
    python flash.py --build          # 先用 docker compose 编译再刷
    python flash.py --baud 460800    # 自定义波特率（默认 921600）

依赖（一次性安装）：
    pip install esptool pyserial esp-idf-monitor
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build"
FLASHER_ARGS = BUILD_DIR / "flasher_args.json"

# ESP 系列常见 USB-Serial VID（Espressif USB-JTAG / CP210x / CH340 / FTDI）
ESP_VIDS = {0x303A, 0x10C4, 0x1A86, 0x0403}


def find_port() -> str | None:
    try:
        from serial.tools import list_ports
    except ImportError:
        sys.exit("缺少 pyserial：pip install pyserial")
    ports = list(list_ports.comports())
    if not ports:
        return None
    # 优先匹配 Espressif / 常见 USB-UART
    matched = [p for p in ports if (p.vid or 0) in ESP_VIDS]
    pool = matched or ports
    if len(pool) == 1:
        return pool[0].device
    print("发现多个串口，请用 --port 指定：")
    for p in pool:
        vid = f"{p.vid:04X}" if p.vid else "----"
        pid = f"{p.pid:04X}" if p.pid else "----"
        print(f"  {p.device:8}  VID:PID={vid}:{pid}  {p.description}")
    return None


def load_flash_args() -> tuple[str, dict, list[tuple[str, str]]]:
    if not FLASHER_ARGS.exists():
        sys.exit(f"找不到 {FLASHER_ARGS}，请先编译（python flash.py --build 或 docker compose run --rm esp-build）")
    data = json.loads(FLASHER_ARGS.read_text(encoding="utf-8"))
    chip = data["extra_esptool_args"]["chip"]
    settings = data["flash_settings"]
    files = sorted(((off, BUILD_DIR / fn) for off, fn in data["flash_files"].items()),
                   key=lambda x: int(x[0], 16))
    files = [(off, str(p)) for off, p in files]
    for _, p in files:
        if not Path(p).exists():
            sys.exit(f"缺少固件文件：{p}")
    return chip, settings, files


def docker_build() -> None:
    if not shutil.which("docker"):
        sys.exit("未找到 docker，可执行：docker compose run --rm esp-build")
    print("==> docker compose run --rm esp-build")
    rc = subprocess.call(["docker", "compose", "run", "--rm", "esp-build"], cwd=ROOT)
    if rc != 0:
        sys.exit(f"docker 编译失败 (rc={rc})")


def run_esptool(*args: str) -> None:
    cmd = [sys.executable, "-m", "esptool", *args]
    print("==>", " ".join(cmd))
    rc = subprocess.call(cmd)
    if rc != 0:
        sys.exit(f"esptool 失败 (rc={rc})")


def run_monitor(port: str, baud: int) -> None:
    cmd = [sys.executable, "-m", "esp_idf_monitor", "-p", port, "-b", str(baud)]
    print("==>", " ".join(cmd), "  (Ctrl-] 退出)")
    try:
        subprocess.call(cmd)
    except KeyboardInterrupt:
        pass


def main() -> int:
    ap = argparse.ArgumentParser(description="ESP32 一键刷写 / 监视")
    ap.add_argument("--port", help="串口（如 COM5 / /dev/ttyUSB0），不传则自动检测")
    ap.add_argument("--baud", type=int, default=921600, help="刷写波特率，默认 921600")
    ap.add_argument("--monitor-baud", type=int, default=115200, help="monitor 波特率，默认 115200")
    ap.add_argument("--erase", action="store_true", help="刷写前先 erase_flash")
    ap.add_argument("--no-monitor", action="store_true", help="刷完不进 monitor")
    ap.add_argument("--monitor-only", action="store_true", help="只 monitor，不刷写")
    ap.add_argument("--build", action="store_true", help="先用 docker compose 编译")
    args = ap.parse_args()

    if args.build:
        docker_build()

    port = args.port or find_port()
    if not port:
        sys.exit("没有自动检测到串口，请用 --port 指定")
    print(f"使用端口: {port}")

    if args.monitor_only:
        run_monitor(port, args.monitor_baud)
        return 0

    chip, settings, files = load_flash_args()
    base = ["--chip", chip, "--port", port, "--baud", str(args.baud),
            "--before", "default_reset", "--after", "hard_reset"]

    if args.erase:
        run_esptool(*base, "erase_flash")

    write = base + [
        "write_flash",
        "--flash_mode", settings["flash_mode"],
        "--flash_size", settings["flash_size"],
        "--flash_freq", settings["flash_freq"],
    ]
    for off, path in files:
        write += [off, path]
    run_esptool(*write)

    if not args.no_monitor:
        run_monitor(port, args.monitor_baud)
    return 0


if __name__ == "__main__":
    sys.exit(main())
