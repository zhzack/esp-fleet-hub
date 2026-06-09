"""Start Docker services for either the dev or prod stack."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.py._common import (  # noqa: E402
    add_stack_args,
    cyn,
    grn,
    is_port_taken_by_docker,
    port_in_use,
    port_pids,
    red,
    require_docker,
    resolve_stack,
    run_compose,
    safe_kill_pid,
    wait_for_urls,
    yel,
)


def main():
    require_docker()
    ap = argparse.ArgumentParser()
    add_stack_args(ap, default="prod")
    ap.add_argument("--kill", action="store_true", help="kill local processes holding stack ports")
    ap.add_argument("--build", action="store_true", help="build app services before up")
    args = ap.parse_args()
    stack = resolve_stack(args)

    for port in stack.exposed_ports:
        if not port_in_use(port):
            continue
        if is_port_taken_by_docker(port):
            yel(f"port {port} is already owned by Docker; compose will take over")
            continue

        pids = port_pids(port)
        yel(f"port {port} is in use by local process(es): {pids or '?'}")
        if args.kill and pids:
            for pid in pids:
                safe_kill_pid(pid)
            time.sleep(1)
            if port_in_use(port) and not is_port_taken_by_docker(port):
                red(f"port {port} is still busy; stop it manually and retry")
                sys.exit(1)
        else:
            red("use --kill or free the port manually before starting the stack")
            sys.exit(1)

    if args.build:
        cyn(f"==> build {stack.name} stack services: {' '.join(stack.build_services)}")
        run_compose(stack, ["build", *stack.build_services])

    cyn(f"==> docker compose up -d ({stack.name})")
    run_compose(stack, ["up", "-d"])

    cyn("==> waiting for stack to become reachable ...")
    if wait_for_urls(stack.ready_urls):
        grn(f"==> {stack.name} stack is ready")
        for url in stack.display_urls:
            print(f"    {url}")
    else:
        red("stack did not become reachable in time; inspect logs for details")
        run_compose(stack, ["ps"], check=False)
        sys.exit(1)


if __name__ == "__main__":
    main()
