"""Show Docker logs for either the dev or prod stack."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.py._common import add_stack_args, require_docker, resolve_stack, run_compose  # noqa: E402


def main():
    require_docker()
    ap = argparse.ArgumentParser()
    add_stack_args(ap, default="prod")
    ap.add_argument("--all", action="store_true", help="show logs for all services in the stack")
    ap.add_argument("--tail", type=int, help="show only the last N lines without follow")
    args = ap.parse_args()
    stack = resolve_stack(args)

    cmd = ["logs"]
    if args.tail is None:
        cmd.extend(["-f", "--tail=100"])
    else:
        cmd.append(f"--tail={args.tail}")
    if not args.all:
        cmd.extend(stack.default_log_services)

    try:
        run_compose(stack, cmd, check=False)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
