"""Build Docker images for either the dev or prod stack."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.py._common import IMAGE_NAME, add_stack_args, cyn, grn, red, require_docker, resolve_stack, run, run_compose  # noqa: E402


def main():
    require_docker()
    ap = argparse.ArgumentParser()
    add_stack_args(ap, default="prod")
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument("--tag", help="extra image tag, only valid for --prod")
    args = ap.parse_args()
    stack = resolve_stack(args)

    if args.tag and stack.name != "prod":
        red("--tag is only supported for the prod stack")
        sys.exit(1)

    cmd = ["build"]
    if args.no_cache:
        cmd.append("--no-cache")
    cmd.extend(stack.build_services)

    cyn(f"==> build {stack.name} stack services: {' '.join(stack.build_services)}")
    run_compose(stack, cmd)

    if args.tag:
        cyn(f"==> add extra tag: {IMAGE_NAME} -> hid-gateway:{args.tag}")
        run(["docker", "tag", IMAGE_NAME, f"hid-gateway:{args.tag}"])

    grn("==> done")
    run(["docker", "images", "--format", "table {{.Repository}}:{{.Tag}}\t{{.ID}}\t{{.Size}}"], check=False)


if __name__ == "__main__":
    main()
