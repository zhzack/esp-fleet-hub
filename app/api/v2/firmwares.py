import hashlib
import os
import shutil
import socket
import uuid
from pathlib import Path
from typing import List, Optional

from fastapi import (
    APIRouter,
    Depends,
    File,
    Form,
    HTTPException,
    Request,
    UploadFile,
)
from fastapi.responses import FileResponse
from sqlalchemy.orm import Session

from app.core.logger.logger import logger
from app.core.security import get_current_user
from app.database.crud import firmware as crud_firmware
from app.database.crud import device as crud_device
from app.database.models.user import User
from app.database.schemas.firmware import (
    FirmwareCreate,
    FirmwareOut,
    OtaProgressOut,
    OtaPushIn,
)
from app.database.session import get_db
from app.services import dispatcher
from app.services.ota_tracker import ota_tracker

router = APIRouter()

UPLOAD_DIR = Path("uploads/firmwares")
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)


def _get_lan_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def _build_download_url(request: Request, firmware_id: int) -> str:
    host = request.headers.get("host") or "127.0.0.1:8000"
    if ":" in host:
        hostname, port = host.split(":", 1)
    else:
        hostname, port = host, "80"

    # 如果是从 localhost 访问，ESP32 无法直接连 localhost，需换成服务器局域网真实 IP
    if hostname in ("127.0.0.1", "localhost", "0.0.0.0"):
        hostname = _get_lan_ip()

    scheme = request.url.scheme or "http"
    return f"{scheme}://{hostname}:{port}/api/v2/firmwares/{firmware_id}/download"


@router.post("/upload", response_model=FirmwareOut)
async def upload_firmware(
    request: Request,
    file: UploadFile = File(...),
    version: str = Form(...),
    description: Optional[str] = Form(None),
    target_chip: str = Form("esp32s3"),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if user.role != "admin":
        raise HTTPException(status_code=403, detail="Admin permission required to upload firmware")

    if not file.filename.endswith(".bin"):
        raise HTTPException(status_code=400, detail="Only .bin firmware files are supported")

    file_uuid = str(uuid.uuid4())[:8]
    safe_filename = f"{file_uuid}_{file.filename}"
    file_path = UPLOAD_DIR / safe_filename

    hasher = hashlib.sha256()
    file_size = 0

    try:
        with open(file_path, "wb") as f:
            while chunk := await file.read(65536):
                f.write(chunk)
                hasher.update(chunk)
                file_size += len(chunk)
    except Exception as e:
        if file_path.exists():
            file_path.unlink()
        logger.exception(f"Failed to save firmware: {e}")
        raise HTTPException(status_code=500, detail=f"Failed to save firmware file: {e}")

    sha256 = hasher.hexdigest()

    firmware_in = FirmwareCreate(
        version=version.strip(),
        filename=file.filename,
        file_path=str(file_path),
        file_size=file_size,
        sha256=sha256,
        description=description,
        target_chip=target_chip,
    )

    firmware = crud_firmware.create_firmware(db, firmware_in)
    out = FirmwareOut.model_validate(firmware)
    out.download_url = _build_download_url(request, firmware.id)
    return out


@router.get("", response_model=List[FirmwareOut])
def list_firmwares(
    request: Request,
    skip: int = 0,
    limit: int = 100,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    firmwares = crud_firmware.get_firmwares(db, skip=skip, limit=limit)
    res = []
    for f in firmwares:
        item = FirmwareOut.model_validate(f)
        item.download_url = _build_download_url(request, f.id)
        res.append(item)
    return res


@router.get("/{firmware_id}", response_model=FirmwareOut)
def get_firmware(
    firmware_id: int,
    request: Request,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    fw = crud_firmware.get_firmware(db, firmware_id)
    if not fw:
        raise HTTPException(status_code=404, detail="Firmware not found")
    out = FirmwareOut.model_validate(fw)
    out.download_url = _build_download_url(request, fw.id)
    return out


@router.delete("/{firmware_id}")
def delete_firmware(
    firmware_id: int,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if user.role != "admin":
        raise HTTPException(status_code=403, detail="Admin permission required")
    fw = crud_firmware.get_firmware(db, firmware_id)
    if not fw:
        raise HTTPException(status_code=404, detail="Firmware not found")

    file_path = Path(fw.file_path)
    if file_path.exists():
        try:
            file_path.unlink()
        except Exception as e:
            logger.warning(f"Could not delete firmware file {file_path}: {e}")

    crud_firmware.delete_firmware(db, firmware_id)
    return {"status": "ok", "message": "Firmware deleted"}


@router.get("/{firmware_id}/download")
def download_firmware(
    firmware_id: int,
    db: Session = Depends(get_db),
):
    fw = crud_firmware.get_firmware(db, firmware_id)
    if not fw:
        raise HTTPException(status_code=404, detail="Firmware not found")
    file_path = Path(fw.file_path)
    if not file_path.exists():
        raise HTTPException(status_code=404, detail="Firmware binary file missing on disk")

    return FileResponse(
        path=str(file_path),
        filename=fw.filename,
        media_type="application/octet-stream",
        headers={
            "Content-Length": str(fw.file_size),
            "X-Firmware-Version": fw.version,
            "X-Firmware-SHA256": fw.sha256,
        },
    )


@router.post("/{firmware_id}/push")
async def push_firmware_ota(
    firmware_id: int,
    payload: OtaPushIn,
    request: Request,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if user.role != "admin":
        raise HTTPException(status_code=403, detail="Admin permission required")

    fw = crud_firmware.get_firmware(db, firmware_id)
    if not fw:
        raise HTTPException(status_code=404, detail="Firmware not found")

    download_url = payload.custom_url or _build_download_url(request, firmware_id)
    logger.info(f"Triggering OTA upgrade with URL: {download_url} for devices: {payload.device_uuids}")

    results = []
    for dev_uuid in payload.device_uuids:
        dev = crud_device.get_by_uuid(db, dev_uuid)
        if not dev:
            results.append({"device_uuid": dev_uuid, "status": "failed", "error": "device not found"})
            continue

        # 初始化 tracker 状态
        ota_tracker.update_progress(
            dev_uuid,
            {
                "status": "starting",
                "progress": 0,
                "bytes_read": 0,
                "total_bytes": fw.file_size,
                "version": fw.version,
            },
        )

        res = await dispatcher.deliver_command(
            db,
            device_uuid=dev_uuid,
            cmd_type="ota.start",
            payload={
                "url": download_url,
                "version": fw.version,
                "sha256": fw.sha256,
            },
            user_id=user.id,
            timeout_ms=5000,
        )
        results.append({"device_uuid": dev_uuid, "dispatch_result": res})

    return {
        "status": "dispatched",
        "firmware_version": fw.version,
        "download_url": download_url,
        "results": results,
    }


@router.get("/progress/{device_uuid}")
def get_ota_progress(
    device_uuid: str,
    user: User = Depends(get_current_user),
):
    p = ota_tracker.get_progress(device_uuid)
    if not p:
        return {
            "device_id": device_uuid,
            "status": "idle",
            "progress": 0,
            "bytes_read": 0,
            "total_bytes": 0,
            "updated_at": 0,
        }
    return p


@router.get("/devices/{device_uuid}/status")
async def get_device_ota_status(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    dev = crud_device.get_by_uuid(db, device_uuid)
    if not dev:
        raise HTTPException(status_code=404, detail="Device not found")

    res = await dispatcher.deliver_command(
        db,
        device_uuid=device_uuid,
        cmd_type="ota.status",
        payload={},
        user_id=user.id,
        timeout_ms=3000,
    )
    return res
