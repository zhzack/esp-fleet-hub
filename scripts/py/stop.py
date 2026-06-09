"""Stop Docker services for either the dev or prod stack."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.py._common import add_stack_args, grn, require_docker, resolve_stack, run_compose  # noqa: E402


def main():
    require_docker()
    ap = argparse.ArgumentParser()
    add_stack_args(ap, default="prod")
    ap.add_argument("--app", action="store_true", help="stop only app services, keep db/cache running")
    ap.add_argument("--rm", action="store_true", help="docker compose down")
    args = ap.parse_args()
    stack = resolve_stack(args)

    if args.app:
        run_compose(stack, ["stop", *stack.app_services])
    elif args.rm:
        run_compose(stack, ["down"])
    else:
        run_compose(stack, ["stop"])

    grn("==> done")
    run_compose(stack, ["ps"], check=False)


if __name__ == "__main__":
    main()
