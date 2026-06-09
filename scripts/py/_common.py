"""Shared helpers for local scripts."""

from __future__ import annotations

import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
APP_PORT = int(os.environ.get("APP_PORT", "8000"))
IMAGE_NAME = os.environ.get("IMAGE_NAME", "hid-gateway:latest")

IS_WIN = sys.platform == "win32"
DEFAULT_STACK = os.environ.get("COMPOSE_STACK", "prod").strip().lower() or "prod"


@dataclass(frozen=True)
class ComposeStack:
    name: str
    compose_file: Path
    app_services: tuple[str, ...]
    build_services: tuple[str, ...]
    default_log_services: tuple[str, ...]
    exposed_ports: tuple[int, ...]
    ready_urls: tuple[str, ...]
    display_urls: tuple[str, ...]
    image_names: tuple[str, ...]


STACKS = {
    "dev": ComposeStack(
        name="dev",
        compose_file=PROJECT_ROOT / "compose.dev.yml",
        app_services=("backend", "frontend"),
        build_services=("backend", "frontend"),
        default_log_services=("backend", "frontend"),
        exposed_ports=(8000, 5173),
        ready_urls=("http://127.0.0.1:8000/api/v1/health", "http://127.0.0.1:5173"),
        display_urls=("http://127.0.0.1:5173", "http://127.0.0.1:8000/docs"),
        image_names=("py_web-backend:latest", "py_web-frontend:latest"),
    ),
    "prod": ComposeStack(
        name="prod",
        compose_file=PROJECT_ROOT / "compose.prod.yml",
        app_services=("app",),
        build_services=("app",),
        default_log_services=("app",),
        exposed_ports=(8000,),
        ready_urls=("http://127.0.0.1:8000/api/v1/health",),
        display_urls=("http://127.0.0.1:8000",),
        image_names=(IMAGE_NAME,),
    ),
}


def _color(code: str, msg: str) -> str:
    if IS_WIN and "ANSICON" not in os.environ:
        try:
            os.system("")
        except Exception:
            pass
    return f"\033[{code}m{msg}\033[0m"


def red(msg: str): print(_color("31", msg))
def grn(msg: str): print(_color("32", msg))
def yel(msg: str): print(_color("33", msg))
def cyn(msg: str): print(_color("36", msg))


def port_in_use(port: int) -> bool:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.3)
    try:
        s.bind(("127.0.0.1", port))
        return False
    except OSError:
        return True
    finally:
        s.close()


def port_pids(port: int) -> list[int]:
    pids: set[int] = set()
    if IS_WIN:
        try:
            out = subprocess.check_output(["netstat", "-ano"], text=True, errors="ignore")
        except Exception:
            return []
        for line in out.splitlines():
            if re.search(rf"[:\.]{port}\s", line) and "LISTEN" in line.upper():
                m = re.search(r"(\d+)\s*$", line.strip())
                if m:
                    pids.add(int(m.group(1)))
    else:
        if shutil.which("ss"):
            try:
                out = subprocess.check_output(
                    ["ss", "-ltnp", f"sport = :{port}"], text=True, errors="ignore"
                )
                for m in re.finditer(r"pid=(\d+)", out):
                    pids.add(int(m.group(1)))
            except Exception:
                pass
        elif shutil.which("lsof"):
            try:
                out = subprocess.check_output(
                    ["lsof", "-iTCP", f"-i:{port}", "-sTCP:LISTEN", "-t"],
                    text=True, errors="ignore",
                )
                for line in out.splitlines():
                    if line.strip().isdigit():
                        pids.add(int(line.strip()))
            except Exception:
                pass
    return sorted(pids)


def kill_pid(pid: int) -> bool:
    try:
        if IS_WIN:
            subprocess.run(
                ["taskkill", "/PID", str(pid), "/F"],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        else:
            os.kill(pid, 9)
        return True
    except Exception:
        return False


_KILL_BLOCKLIST = re.compile(
    r"docker|vpnkit|wsl|com\.docker|svchost|services\.exe|System",
    re.IGNORECASE,
)
_KILL_ALLOWLIST = re.compile(r"python|uvicorn|node", re.IGNORECASE)


def pid_name(pid: int) -> str:
    try:
        if IS_WIN:
            out = subprocess.check_output(
                ["tasklist", "/FI", f"PID eq {pid}", "/NH", "/FO", "CSV"],
                text=True,
                errors="ignore",
            ).strip()
            if out:
                return out.split('","')[0].lstrip('"')
        else:
            with open(f"/proc/{pid}/comm") as f:
                return f.read().strip()
    except Exception:
        pass
    return "unknown"


def safe_kill_pid(pid: int) -> bool:
    name = pid_name(pid)
    if _KILL_BLOCKLIST.search(name):
        red(f"  !! refusing to kill PID={pid} name={name} (protected process)")
        return False
    if not _KILL_ALLOWLIST.search(name):
        red(f"  !! skipping PID={pid} name={name} (non-app process)")
        return False
    yel(f"  -> kill PID={pid} name={name}")
    return kill_pid(pid)


def require_docker():
    if not shutil.which("docker"):
        red("docker is not installed or not in PATH")
        sys.exit(1)


def run(cmd: list[str], cwd: Path | None = None, check: bool = True) -> int:
    cyn(f"$ {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=str(cwd or PROJECT_ROOT), check=check).returncode


def add_stack_args(ap, *, default: str | None = None):
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--dev", action="store_true", help="use compose.dev.yml")
    group.add_argument("--prod", action="store_true", help="use compose.prod.yml")
    ap.set_defaults(_stack_default=(default or DEFAULT_STACK))


def resolve_stack(args, *, default: str | None = None) -> ComposeStack:
    stack_name = default or getattr(args, "_stack_default", DEFAULT_STACK)
    if getattr(args, "dev", False):
        stack_name = "dev"
    elif getattr(args, "prod", False):
        stack_name = "prod"
    stack_name = (stack_name or DEFAULT_STACK).lower()
    if stack_name not in STACKS:
        raise ValueError(f"unknown compose stack: {stack_name}")
    return STACKS[stack_name]


def compose_cmd(stack: ComposeStack) -> list[str]:
    return ["docker", "compose", "-f", str(stack.compose_file)]


def run_compose(
    stack: ComposeStack,
    args: list[str],
    *,
    cwd: Path | None = None,
    check: bool = True,
) -> int:
    return run(compose_cmd(stack) + args, cwd=cwd, check=check)


def is_port_taken_by_docker(port: int) -> bool:
    try:
        out = subprocess.check_output(
            ["docker", "ps", "--filter", f"publish={port}", "--format", "{{.Names}}"],
            text=True,
            errors="ignore",
        )
        return bool(out.strip())
    except Exception:
        return False


def url_ready(url: str) -> bool:
    try:
        with urllib.request.urlopen(url, timeout=2) as resp:
            return 200 <= resp.status < 500
    except (urllib.error.URLError, TimeoutError, ValueError):
        return False


def wait_for_urls(urls: tuple[str, ...], timeout: int = 90) -> bool:
    deadline = time.time() + timeout
    pending = set(urls)
    while time.time() < deadline:
        for url in list(pending):
            if url_ready(url):
                pending.remove(url)
        if not pending:
            return True
        print(".", end="", flush=True)
        time.sleep(1)
    print()
    return False
