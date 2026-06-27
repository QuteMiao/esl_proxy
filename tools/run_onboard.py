#!/usr/bin/env python3
"""Deprecated wrapper — use tools/run_onboard.sh (no simpler dependency)."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "run_onboard.sh"


def main() -> None:
    p = argparse.ArgumentParser(description="Run esl_proxy onboard smoke (shell backend)")
    p.add_argument("-d", "--device", type=int, default=0)
    p.add_argument("--skip-build", action="store_true")
    args = p.parse_args()

    cmd = ["bash", str(SCRIPT)]
    if args.skip_build:
        cmd.append("--skip-build")
    cmd.extend(["-d", str(args.device)])
    if os.environ.get("TASK_DEVICE"):
        cmd.extend(["-d", os.environ["TASK_DEVICE"]])

    raise SystemExit(subprocess.call(cmd))


if __name__ == "__main__":
    main()
