"""Build firmware in Docker and flash or monitor from the host OS.

Recommended workflow on Windows:
1. Build with Docker so ESP-IDF stays inside the container.
2. Flash and monitor from the host so COM port passthrough is not required.

Host dependencies:
    pip install esptool pyserial esp-idf-monitor
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build"
COMPOSE_FILE = ROOT / "docker-compose.yml"
FLASHER_ARGS = BUILD_DIR / "flasher_args.json"
DEFAULT_IDF_IMAGE = os.environ.get("ESP_IDF_IMAGE", "espressif/idf:release-v5.4")
ESP_VIDS = {0x303A, 0x10C4, 0x1A86, 0x0403}


@dataclass(frozen=True)
class FlashArtifacts:
    chip: str
    before: str
    after: str
    stub: bool
    write_flash_args: tuple[str, ...]
    flash_files: tuple[tuple[str, Path], ...]
    elf_file: Path | None


def format_cmd(cmd: list[str]) -> str:
    if os.name == "nt":
        return subprocess.list2cmdline(cmd)
    return " ".join(cmd)


def require_module(module: str, package_hint: str) -> None:
    if importlib.util.find_spec(module) is None:
        sys.exit(f"Missing Python package for `{module}`. Install with: pip install {package_hint}")


def get_ports():
    require_module("serial", "pyserial")
    from serial.tools import list_ports

    return list(list_ports.comports())


def print_ports() -> None:
    ports = get_ports()
    if not ports:
        print("No serial ports detected.")
        return

    print("Detected serial ports:")
    for port in ports:
        vid = f"{port.vid:04X}" if port.vid is not None else "----"
        pid = f"{port.pid:04X}" if port.pid is not None else "----"
        print(f"  {port.device:10} VID:PID={vid}:{pid}  {port.description}")


def touch_1200bps(port_name: str) -> None:
    require_module("serial", "pyserial")
    import time
    import serial

    print(f"[*] Detected running CDC ({port_name}). Triggering 1200bps touch to enter ROM bootloader...")
    try:
        s = serial.Serial(port_name, 1200)
        s.close()
    except Exception:
        pass
    time.sleep(1.8)


def find_port() -> str | None:
    ports = get_ports()
    if not ports:
        return None

    # Auto 1200bps touch if ESP32 TinyUSB composite CDC (303A:400C) is active
    for port in ports:
        if (port.vid or 0) == 0x303A and (port.pid or 0) == 0x400C:
            touch_1200bps(port.device)
            ports = get_ports()
            break

    matched = [port for port in ports if (port.vid or 0) in ESP_VIDS]
    pool = matched or ports
    if len(pool) == 1:
        return pool[0].device

    print("Multiple serial ports detected. Use --port to select one.")
    for port in pool:
        vid = f"{port.vid:04X}" if port.vid is not None else "----"
        pid = f"{port.pid:04X}" if port.pid is not None else "----"
        print(f"  {port.device:10} VID:PID={vid}:{pid}  {port.description}")
    return None


def artifact_issues() -> list[str]:
    issues: list[str] = []
    if not FLASHER_ARGS.exists():
        return [f"Missing {FLASHER_ARGS}"]

    try:
        data = json.loads(FLASHER_ARGS.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [f"Invalid {FLASHER_ARGS}: {exc}"]

    flash_files = data.get("flash_files", {})
    if not flash_files:
        issues.append(f"No flash files declared in {FLASHER_ARGS}")
        return issues

    for offset, rel_path in flash_files.items():
        file_path = BUILD_DIR / rel_path
        if not file_path.exists():
            issues.append(f"Missing {file_path} for offset {offset}")
    return issues


def find_elf_file() -> Path | None:
    elf_candidates = [BUILD_DIR / f"{ROOT.name}.elf", BUILD_DIR / "project.elf"]
    return next((path for path in elf_candidates if path.exists()), None)


def load_flash_artifacts() -> FlashArtifacts:
    issues = artifact_issues()
    if issues:
        sys.exit("\n".join(issues))

    data = json.loads(FLASHER_ARGS.read_text(encoding="utf-8"))
    extra = data.get("extra_esptool_args", {})
    write_flash_args = tuple(data.get("write_flash_args", ()))
    if not write_flash_args:
        settings = data.get("flash_settings", {})
        write_flash_args = (
            "--flash_mode",
            settings["flash_mode"],
            "--flash_size",
            settings["flash_size"],
            "--flash_freq",
            settings["flash_freq"],
        )

    flash_files = tuple(
        sorted(
            ((offset, BUILD_DIR / rel_path) for offset, rel_path in data["flash_files"].items()),
            key=lambda item: int(item[0], 16),
        )
    )

    return FlashArtifacts(
        chip=extra.get("chip", "auto"),
        before=extra.get("before", "default_reset"),
        after=extra.get("after", "hard_reset"),
        stub=bool(extra.get("stub", True)),
        write_flash_args=write_flash_args,
        flash_files=flash_files,
        elf_file=find_elf_file(),
    )


def compose_cmd(*args: str) -> list[str]:
    return ["docker", "compose", "-f", str(COMPOSE_FILE), *args]


def docker_env(idf_image: str) -> dict[str, str]:
    env = os.environ.copy()
    env["ESP_IDF_IMAGE"] = idf_image
    return env


def require_docker_ready() -> None:
    if not shutil.which("docker"):
        sys.exit("Docker is not installed or not in PATH.")

    result = subprocess.run(
        ["docker", "info"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=20,
        check=False,
    )
    if result.returncode != 0:
        message = result.stdout.strip() or "docker info failed"
        sys.exit(
            "Docker is not ready. Start Docker Desktop or the Docker daemon and retry.\n"
            f"{message}"
        )


def run_logged(cmd: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> tuple[int, str]:
    print(f"==> {format_cmd(cmd)}")
    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd or ROOT),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )

    assert proc.stdout is not None
    output: list[str] = []
    for line in proc.stdout:
        print(line, end="")
        output.append(line)

    return proc.wait(), "".join(output)


def fail_docker_step(step: str, rc: int, output: str, idf_image: str) -> None:
    lower = output.lower()
    hints = [f"{step} failed (rc={rc})."]

    if any(token in lower for token in ("unexpected eof", "failed size validation", "short read")):
        hints.append(
            "Docker failed while pulling the ESP-IDF base image. This usually points to a broken registry mirror, proxy, or cached layer."
        )
    else:
        hints.append("See the Docker output above for the exact failure.")

    hints.append(f"ESP_IDF_IMAGE={idf_image}")
    hints.append("You can override the base image with `--idf-image <image>` or the `ESP_IDF_IMAGE` environment variable.")
    hints.append("If Docker Desktop uses a registry mirror or proxy, verify it or disable it and retry.")
    sys.exit("\n".join(hints))


def docker_build(*, idf_image: str, no_cache: bool) -> None:
    require_docker_ready()
    env = docker_env(idf_image)

    build_cmd = compose_cmd("build")
    if no_cache:
        build_cmd.append("--no-cache")
    build_cmd.append("esp-build")
    rc, output = run_logged(build_cmd, cwd=ROOT, env=env)
    if rc != 0:
        fail_docker_step("docker compose build esp-build", rc, output, idf_image)

    rc, output = run_logged(compose_cmd("run", "--rm", "esp-build"), cwd=ROOT, env=env)
    if rc != 0:
        fail_docker_step("docker compose run --rm esp-build", rc, output, idf_image)


def ensure_flash_artifacts(args: argparse.Namespace) -> None:
    if args.monitor_only:
        return

    issues = artifact_issues()
    if not issues:
        return

    if args.no_auto_build:
        sys.exit("\n".join(issues))

    print("Flash artifacts are missing. Building firmware in Docker first.")
    docker_build(idf_image=args.idf_image, no_cache=args.docker_no_cache)


def run_esptool(*args: str) -> None:
    require_module("esptool", "esptool")
    cmd = [sys.executable, "-m", "esptool", *args]
    print(f"==> {format_cmd(cmd)}")
    rc = subprocess.call(cmd)
    if rc != 0:
        sys.exit(f"esptool failed (rc={rc})")


def run_monitor(port: str, baud: int, elf_file: Path | None) -> None:
    require_module("esp_idf_monitor", "esp-idf-monitor")
    cmd = [sys.executable, "-m", "esp_idf_monitor", "-p", port, "-b", str(baud)]
    if elf_file is not None:
        cmd.append(str(elf_file))
    print(f"==> {format_cmd(cmd)}  (exit with Ctrl+])")
    try:
        subprocess.call(cmd)
    except KeyboardInterrupt:
        pass


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build firmware in Docker, then flash and monitor from the host."
    )
    parser.add_argument("--port", help="Serial port such as COM5 or /dev/ttyUSB0.")
    parser.add_argument("--baud", type=int, default=921600, help="Flash baud rate. Default: 921600.")
    parser.add_argument("--monitor-baud", type=int, default=115200, help="Monitor baud rate. Default: 115200.")
    parser.add_argument("--erase", action="store_true", help="Run erase_flash before write_flash.")
    parser.add_argument("--no-monitor", action="store_true", help="Flash only. Do not start the serial monitor.")
    parser.add_argument("--monitor-only", action="store_true", help="Start the serial monitor without flashing.")
    parser.add_argument("--list-ports", action="store_true", help="List detected serial ports and exit.")
    parser.add_argument("--build", action="store_true", help="Always build the firmware in Docker before continuing.")
    parser.add_argument("--build-only", action="store_true", help="Build the firmware in Docker and exit.")
    parser.add_argument("--no-auto-build", action="store_true", help="Fail instead of auto-building when artifacts are missing.")
    parser.add_argument("--docker-no-cache", action="store_true", help="Use `docker compose build --no-cache` for the ESP-IDF image.")
    parser.add_argument("--idf-image", default=DEFAULT_IDF_IMAGE, help=f"ESP-IDF base image. Default: {DEFAULT_IDF_IMAGE}")
    args = parser.parse_args()

    if args.monitor_only and args.erase:
        parser.error("--erase cannot be used with --monitor-only")
    if args.monitor_only and args.build_only:
        parser.error("--monitor-only cannot be used with --build-only")

    if args.list_ports:
        print_ports()
        return 0

    if args.build or args.build_only:
        docker_build(idf_image=args.idf_image, no_cache=args.docker_no_cache)
        if args.build_only:
            return 0

    ensure_flash_artifacts(args)

    port = args.port or find_port()
    if not port:
        sys.exit("No serial port was selected. Use --port or run with --list-ports.")
    print(f"Using port: {port}")

    if args.monitor_only:
        run_monitor(port, args.monitor_baud, find_elf_file())
        return 0

    artifacts = load_flash_artifacts()
    base = [
        "--chip",
        artifacts.chip,
        "--port",
        port,
        "--baud",
        str(args.baud),
        "--before",
        artifacts.before,
        "--after",
        artifacts.after,
    ]
    if not artifacts.stub:
        base.append("--no-stub")

    if args.erase:
        run_esptool(*base, "erase_flash")

    write_cmd = [*base, "write_flash", *artifacts.write_flash_args]
    for offset, path in artifacts.flash_files:
        write_cmd.extend([offset, str(path)])
    run_esptool(*write_cmd)

    if not args.no_monitor:
        run_monitor(port, args.monitor_baud, artifacts.elf_file)
    return 0


if __name__ == "__main__":
    sys.exit(main())
