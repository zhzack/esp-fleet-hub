"""Clean Docker resources for either the dev or prod stack."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.py._common import add_stack_args, cyn, grn, red, require_docker, resolve_stack, run, run_compose, yel  # noqa: E402


def main():
    require_docker()
    ap = argparse.ArgumentParser()
    add_stack_args(ap, default="prod")
    ap.add_argument("--image", action="store_true")
    ap.add_argument("--volumes", action="store_true")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()
    stack = resolve_stack(args)

    del_img = args.image or args.all
    del_vol = args.volumes or args.all

    if del_vol:
        yel(f"!! this will delete {stack.name} stack volumes and database/cache data")
        if input('type "yes" to continue: ').strip() != "yes":
            red("cancelled")
            sys.exit(1)
        run_compose(stack, ["down", "-v"])
    else:
        run_compose(stack, ["down"])

    if del_img:
        for image in stack.image_names:
            cyn(f"==> remove image {image}")
            run(["docker", "rmi", "-f", image], check=False)

    grn("==> done")


if __name__ == "__main__":
    main()
