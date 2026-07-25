from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

import requests


TERMINAL_STATES = {"success", "failed", "fault"}


def normalize_device_url(url: str) -> str:
    url = url.strip()
    if not url.startswith(("http://", "https://")):
        url = "http://" + url
    return url.rstrip("/")


def validate_hex_file(path: Path) -> None:
    if not path.exists():
        raise SystemExit(f"HEX file not found: {path}")

    if not path.is_file():
        raise SystemExit(f"HEX path is not a file: {path}")

    saw_eof = False

    with path.open("r", encoding="utf-8", errors="replace") as file:
        for line_number, raw_line in enumerate(file, start=1):
            line = raw_line.strip()

            if not line:
                continue

            if not line.startswith(":"):
                raise SystemExit(f"Invalid HEX line {line_number}: missing ':'")

            try:
                record = bytes.fromhex(line[1:])
            except ValueError:
                raise SystemExit(f"Invalid HEX line {line_number}: non-hex characters")

            if len(record) < 5:
                raise SystemExit(f"Invalid HEX line {line_number}: record too short")

            byte_count = record[0]
            record_type = record[3]

            expected_len = byte_count + 5
            if len(record) != expected_len:
                raise SystemExit(
                    f"Invalid HEX line {line_number}: expected {expected_len} bytes, got {len(record)}"
                )

            checksum_total = sum(record) & 0xFF
            if checksum_total != 0:
                raise SystemExit(f"Invalid HEX line {line_number}: checksum failed")

            if record_type == 0x01:
                saw_eof = True

    if not saw_eof:
        raise SystemExit("Invalid HEX file: missing EOF record")


def compile_sketch(sketch_path: Path, fqbn: str, output_dir: Path) -> Path:
    if not sketch_path.exists():
        raise SystemExit(f"Sketch path not found: {sketch_path}")

    output_dir.mkdir(parents=True, exist_ok=True)

    command = [
        "arduino-cli",
        "compile",
        "-b",
        fqbn,
        "--output-dir",
        str(output_dir),
        str(sketch_path),
    ]

    print(f"Compiling sketch for {fqbn}...")

    try:
        subprocess.run(command, check=True)
    except FileNotFoundError:
        raise SystemExit("arduino-cli not found. Install Arduino CLI or use --hex.")
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"arduino-cli compile failed with exit code {exc.returncode}")

    hex_files = sorted(output_dir.glob("*.hex"))
    if not hex_files:
        hex_files = sorted(output_dir.rglob("*.hex"))

    if not hex_files:
        raise SystemExit(f"No .hex file found in {output_dir}")

    return hex_files[0]


def get_status(base_url: str) -> dict[str, Any]:
    response = requests.get(f"{base_url}/api/v1/status", timeout=5)
    response.raise_for_status()
    return response.json()


def upload_hex(
    base_url: str,
    hex_path: Path,
    board: str,
    reset: bool,
    verify: bool,
) -> str:
    with hex_path.open("rb") as file:
        files = {
            "file": (hex_path.name, file, "text/plain"),
        }
        data = {
            "board": board,
            "reset": "true" if reset else "false",
            "verify": "true" if verify else "false",
        }

        response = requests.post(
            f"{base_url}/api/v1/upload",
            files=files,
            data=data,
            timeout=30,
        )

    response.raise_for_status()
    payload = response.json()

    if "job_id" not in payload:
        raise SystemExit(f"Bench returned no job_id: {payload}")

    return payload["job_id"]


def poll_upload(base_url: str, job_id: str, poll_interval: float) -> int:
    last_line = None

    while True:
        response = requests.get(f"{base_url}/api/v1/upload/{job_id}", timeout=5)
        response.raise_for_status()
        payload = response.json()

        state = payload.get("state", "unknown")
        progress = int(payload.get("progress", 0))
        message = payload.get("message", "")

        line = f"[{state}] {progress:3}% {message}"

        if line != last_line:
            print(line)
            last_line = line

        if state in TERMINAL_STATES:
            if state == "success":
                print("Upload complete.")
                return 0

            error = payload.get("error") or "UNKNOWN_ERROR"
            print(f"Upload failed: {error}", file=sys.stderr)
            return 1

        time.sleep(poll_interval)


def cmd_status(args: argparse.Namespace) -> int:
    base_url = normalize_device_url(args.device)
    status = get_status(base_url)

    print(f"Bench device: {status.get('device')}")
    print(f"Version:      {status.get('version')}")
    print(f"Mode:         {status.get('mode')}")
    print(f"State:        {status.get('state')}")

    target = status.get("target", {})
    print(f"Target:       {target.get('name') if target.get('present') else 'not present'}")

    power = status.get("power", {})
    print(f"Power:        enabled={power.get('enabled')} fault={power.get('fault')}")

    upload = status.get("upload", {})
    print(f"Upload:       active={upload.get('active')} progress={upload.get('progress')}%")
    print(f"Message:      {upload.get('message')}")

    return 0


def cmd_upload(args: argparse.Namespace) -> int:
    base_url = normalize_device_url(args.device)

    if args.hex:
        hex_path = Path(args.hex).resolve()
        validate_hex_file(hex_path)
    else:
        sketch_path = Path(args.sketch).resolve()
        build_dir = Path(args.build_dir).resolve()
        hex_path = compile_sketch(sketch_path, args.fqbn, build_dir)
        validate_hex_file(hex_path)

    print(f"HEX: {hex_path}")
    print(f"Connecting to Bench at {base_url}...")

    status = get_status(base_url)
    print(f"Bench state: {status.get('state', 'unknown')}")

    job_id = upload_hex(
        base_url=base_url,
        hex_path=hex_path,
        board=args.board,
        reset=not args.no_reset,
        verify=not args.no_verify,
    )

    print(f"Upload job: {job_id}")

    return poll_upload(
        base_url=base_url,
        job_id=job_id,
        poll_interval=args.poll_interval,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="bench",
        description="Bench upload CLI",
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--device",
        default="http://bench.local",
        help="Bench device URL, e.g. http://bench.local or http://localhost:8080",
    )

    status_parser = subparsers.add_parser(
        "status",
        parents=[common],
        help="Show Bench device status",
    )
    status_parser.set_defaults(func=cmd_status)

    upload_parser = subparsers.add_parser(
        "upload",
        parents=[common],
        help="Compile/send a sketch or upload a .hex file",
    )
    upload_parser.add_argument(
        "sketch",
        nargs="?",
        help="Arduino sketch folder",
    )
    upload_parser.add_argument(
        "--hex",
        help="Path to a precompiled Intel HEX file",
    )
    upload_parser.add_argument(
        "--fqbn",
        default="arduino:avr:uno",
        help="Arduino CLI board FQBN",
    )
    upload_parser.add_argument(
        "--board",
        default="uno",
        help="Bench target board profile",
    )
    upload_parser.add_argument(
        "--build-dir",
        default=".bench-build",
        help="Arduino CLI output directory",
    )
    upload_parser.add_argument(
        "--no-reset",
        action="store_true",
        help="Do not ask Bench to reset the target",
    )
    upload_parser.add_argument(
        "--no-verify",
        action="store_true",
        help="Do not ask Bench to verify after upload",
    )
    upload_parser.add_argument(
        "--poll-interval",
        type=float,
        default=0.25,
        help="Upload status polling interval in seconds",
    )
    upload_parser.set_defaults(func=cmd_upload)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "upload" and not args.hex and not args.sketch:
        parser.error("upload requires either a sketch folder or --hex")

    try:
        return args.func(args)
    except requests.HTTPError as exc:
        print(f"HTTP error: {exc}", file=sys.stderr)
        if exc.response is not None:
            print(exc.response.text, file=sys.stderr)
        return 1
    except requests.RequestException as exc:
        print(f"Connection error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())