#!/usr/bin/env python3
"""Build and run the deterministic DolRecomp/RecompCore opcode oracle.

The generated DOL contains no copyrighted game data. ModernGekko recompiles it
with the normal standalone DolRecomp path, then RecompCore executes every block
natively and replays it through Dolphin's interpreter from the same state.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import queue
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[1]
RESULT_PREFIX = "[opfuzz] RESULT "


def parse_result(line: str) -> dict[str, str] | None:
    if RESULT_PREFIX not in line:
        return None
    payload = line.split(RESULT_PREFIX, 1)[1].strip()
    values: dict[str, str] = {}
    for item in payload.split():
        if "=" in item:
            key, value = item.split("=", 1)
            values[key] = value
    return values


def result_passed(values: dict[str, str]) -> bool:
    required_zero = (
        "divergent_blocks",
        "reports",
        "fallback_skips",
        "zero_charges",
        "rejected_blocks",
    )
    try:
        return int(values.get("dispatches", "0"), 0) > 0 and all(
            int(values[name], 0) == 0 for name in required_zero
        )
    except (KeyError, ValueError):
        return False


def write_boot_bin(path: Path) -> None:
    boot = bytearray(0x60)
    boot[0:6] = b"OPFZ01"
    struct.pack_into(">I", boot, 0x1C, 0xC2339F3D)
    name = b"ModernGekko Opcode Oracle"
    boot[0x20 : 0x20 + len(name)] = name
    path.write_bytes(boot)


def run_checked(command: list[str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def stream_until_result(
    command: list[str], env: dict[str, str], timeout: float
) -> tuple[dict[str, str] | None, list[str]]:
    print("+", " ".join(command), flush=True)
    process = subprocess.Popen(
        command,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    lines: queue.Queue[str | None] = queue.Queue()

    def read_output() -> None:
        for output_line in process.stdout:
            lines.put(output_line)
        lines.put(None)

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    deadline = time.monotonic() + timeout
    result: dict[str, str] | None = None
    captured: list[str] = []
    try:
        while time.monotonic() < deadline:
            try:
                line = lines.get(timeout=min(0.25, max(0.01, deadline - time.monotonic())))
            except queue.Empty:
                if process.poll() is not None:
                    break
                continue
            if line is None:
                break
            print(line, end="")
            captured.append(line)
            parsed = parse_result(line)
            if parsed is not None:
                result = parsed
                break
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        reader.join(timeout=1)
    return result, captured


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-upstream-layered")
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=Path(tempfile.gettempdir()) / "moderngekko-opfuzz",
    )
    parser.add_argument("--per-op", type=int, default=1)
    parser.add_argument("--seeds", type=int, default=8)
    parser.add_argument("--generator-seed", type=lambda x: int(x, 0), default=0xC0FFEE)
    parser.add_argument("--state-seed", type=lambda x: int(x, 0), default=0x9E3779B97F4A7C15)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--clean", action="store_true", help="remove the disposable bank and module cache")
    args = parser.parse_args()

    if args.per_op < 1 or args.seeds < 1:
        parser.error("--per-op and --seeds must be positive")

    generator = ROOT / "vendor/dolphin/DolRecomp/tools/gen_opfuzz_dol.py"
    port = args.build_dir / ("moderngekko-port.exe" if os.name == "nt" else "moderngekko-port")
    runner = args.build_dir / ("moderngekko-run.exe" if os.name == "nt" else "moderngekko-run")
    for required in (generator, port, runner):
        if not required.is_file():
            print(f"missing required artifact: {required}", file=sys.stderr)
            return 2

    game_root = args.work_dir / "game"
    cache = args.work_dir / "module-cache"
    user_dir = args.work_dir / "user"
    if args.clean:
        shutil.rmtree(args.work_dir, ignore_errors=True)
    (game_root / "sys").mkdir(parents=True, exist_ok=True)
    (game_root / "files").mkdir(parents=True, exist_ok=True)
    user_dir.mkdir(parents=True, exist_ok=True)
    manifest = args.work_dir / "opfuzz_manifest.txt"
    symbols = args.work_dir / "opfuzz_symbols.txt"
    dol = game_root / "sys" / "main.dol"
    write_boot_bin(game_root / "sys" / "boot.bin")

    run_checked(
        [
            sys.executable,
            str(generator),
            "--out",
            str(dol),
            "--symbols",
            str(symbols),
            "--manifest",
            str(manifest),
            "--per-op",
            str(args.per_op),
            "--seed",
            str(args.generator_seed),
        ]
    )
    run_checked(
        [
            str(port),
            "build",
            str(game_root),
            "--output",
            str(cache),
            "--fast-build",
        ]
    )

    active_module = cache / "OPFZ01" / "active-module.txt"
    if not active_module.is_file():
        print(f"module build did not publish {active_module}", file=sys.stderr)
        return 2
    module = Path(active_module.read_text(encoding="utf-8").strip())
    if not module.is_file():
        print(f"published module is missing: {module}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env.update(
        {
            "STATICRECOMP_LOCKSTEP": "1",
            "STATICRECOMP_LOCKSTEP_MAXREPORT": "64",
            "STATICRECOMP_OPFUZZ": str(args.seeds),
            "STATICRECOMP_OPFUZZ_MANIFEST": str(manifest),
            "STATICRECOMP_OPFUZZ_DOL": str(dol),
            "STATICRECOMP_OPFUZZ_SEED": hex(args.state_seed),
        }
    )
    result, _ = stream_until_result(
        [
            str(runner),
            "--game",
            str(game_root),
            "--module",
            str(module),
            "--user-dir",
            str(user_dir),
            "--headless",
        ],
        env,
        args.timeout,
    )
    if result is None:
        print(f"opfuzz produced no result within {args.timeout:.1f}s", file=sys.stderr)
        return 1
    if not result_passed(result):
        print("opfuzz FAILED", file=sys.stderr)
        return 1
    print("opfuzz PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
