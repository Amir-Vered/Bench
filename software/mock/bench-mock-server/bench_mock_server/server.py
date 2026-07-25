from __future__ import annotations

import argparse
import asyncio
import time
import uuid
from dataclasses import dataclass, field
from typing import Any

import uvicorn
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from pydantic import BaseModel


app = FastAPI(title="Bench Mock Server", version="0.1.0")

START_TIME = time.time()


@dataclass
class UploadJob:
    job_id: str
    state: str = "receiving"
    progress: int = 0
    message: str = "receiving upload"
    error: str | None = None
    created_at: float = field(default_factory=time.time)


class PowerRequest(BaseModel):
    enabled: bool


jobs: dict[str, UploadJob] = {}
logs: list[str] = ["[0.000] Bench mock boot"]

power_enabled = True
power_fault = False
target_present = True


def log(message: str) -> None:
    elapsed = time.time() - START_TIME
    logs.append(f"[{elapsed:0.3f}] {message}")


def validate_hex_bytes(data: bytes) -> None:
    text = data.decode("utf-8", errors="replace")
    lines = [line.strip() for line in text.splitlines() if line.strip()]

    if not lines:
        raise ValueError("empty HEX file")

    for line_number, line in enumerate(lines, start=1):
        if not line.startswith(":"):
            raise ValueError(f"line {line_number} does not start with ':'")

        try:
            bytes.fromhex(line[1:])
        except ValueError as exc:
            raise ValueError(f"line {line_number} contains non-hex characters") from exc


async def simulate_upload(job: UploadJob, verify: bool, reset: bool) -> None:
    if not power_enabled:
        job.state = "failed"
        job.error = "POWER_DISABLED"
        job.message = "target power is disabled"
        return

    if power_fault:
        job.state = "fault"
        job.error = "POWER_FAULT"
        job.message = "target power fault"
        return

    if not target_present:
        job.state = "failed"
        job.error = "NO_TARGET"
        job.message = "target not detected"
        return

    job.state = "target_detected"
    job.progress = 5
    job.message = "Arduino Uno R3 target detected"
    log(f"{job.job_id}: target detected")
    await asyncio.sleep(0.4)

    if reset:
        job.state = "resetting_target"
        job.progress = 10
        job.message = "resetting target into bootloader"
        log(f"{job.job_id}: reset target")
        await asyncio.sleep(0.4)

    job.state = "uploading"
    for progress in range(15, 96, 5):
        job.progress = progress
        job.message = f"writing flash"
        await asyncio.sleep(0.12)

    if verify:
        job.state = "verifying"
        job.progress = 97
        job.message = "verifying flash"
        log(f"{job.job_id}: verifying")
        await asyncio.sleep(0.5)

    job.state = "success"
    job.progress = 100
    job.message = "upload complete"
    log(f"{job.job_id}: success")


@app.get("/api/v1/status")
def get_status() -> dict[str, Any]:
    active_job = next(
        (job for job in jobs.values() if job.state not in {"success", "failed", "fault"}),
        None,
    )

    return {
        "device": "bench",
        "version": "0.1.0",
        "mode": "mock",
        "state": active_job.state if active_job else "idle",
        "target": {
            "present": target_present,
            "name": "Arduino Uno R3" if target_present else None,
        },
        "power": {
            "enabled": power_enabled,
            "fault": power_fault,
        },
        "upload": {
            "active": active_job is not None,
            "job_id": active_job.job_id if active_job else None,
            "progress": active_job.progress if active_job else 0,
            "message": active_job.message if active_job else "idle",
        },
    }


@app.post("/api/v1/upload")
async def upload_hex(
    file: UploadFile = File(...),
    board: str = Form("uno"),
    reset: bool = Form(True),
    verify: bool = Form(True),
) -> dict[str, str]:
    if board != "uno":
        raise HTTPException(status_code=400, detail="mock currently supports board=uno only")

    data = await file.read()

    try:
        validate_hex_bytes(data)
    except ValueError as exc:
        raise HTTPException(
            status_code=400,
            detail={
                "error": "INVALID_HEX",
                "message": str(exc),
            },
        )

    job_id = f"upload-{uuid.uuid4().hex[:8]}"
    job = UploadJob(job_id=job_id)
    jobs[job_id] = job

    log(f"{job_id}: accepted {file.filename} ({len(data)} bytes)")

    asyncio.create_task(simulate_upload(job, verify=verify, reset=reset))

    return {
        "job_id": job_id,
        "state": job.state,
        "message": "upload accepted",
    }


@app.get("/api/v1/upload/{job_id}")
def get_upload_status(job_id: str) -> dict[str, Any]:
    job = jobs.get(job_id)

    if job is None:
        raise HTTPException(status_code=404, detail="job not found")

    return {
        "job_id": job.job_id,
        "state": job.state,
        "progress": job.progress,
        "message": job.message,
        "error": job.error,
    }


@app.post("/api/v1/target/power")
def set_target_power(request: PowerRequest) -> dict[str, bool]:
    global power_enabled

    power_enabled = request.enabled
    log(f"target power {'enabled' if power_enabled else 'disabled'}")

    return {
        "enabled": power_enabled,
        "fault": power_fault,
    }


@app.post("/api/v1/target/reset")
def reset_target() -> dict[str, Any]:
    if not power_enabled:
        raise HTTPException(status_code=400, detail={"error": "POWER_DISABLED"})

    if not target_present:
        raise HTTPException(status_code=400, detail={"error": "NO_TARGET"})

    log("target reset requested")

    return {
        "ok": True,
        "message": "target reset requested",
    }


@app.get("/api/v1/logs")
def get_logs() -> dict[str, list[str]]:
    return {
        "lines": logs[-200:],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Bench mock server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    uvicorn.run(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()