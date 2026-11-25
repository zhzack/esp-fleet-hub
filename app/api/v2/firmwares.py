import hashlib
from pathlib import Path
from typing import List, Optional
import uuid

from fastapi import (
    APIRouter,
    Depends,
    File,
    Form,
    HTTPException,
    Query,
    Request,
    UploadFile,
)
from fastapi.responses import FileResponse
from sqlalchemy.orm import Session

from app.core.logger.logger import logger
from app.core.security import ensure_device_access, get_current_user, get_optional_user
from app.database.crud import firmware as crud_firmware
from app.database.crud import device as crud_device
from app.database.models.unified_fleet import UnifiedDevice
from app.database.models.user import User
from app.database.schemas.firmware import (
    FirmwareCreate,
    FirmwareOut,
    OtaProgressOut,
    OtaPushIn,
)
from app.database.session import get_db
from app.services import dispatcher
from app.services.firmware_utils import (
    build_signed_download_url,
    check_chip_compatibility,
    require_download_auth,
)
from app.services.ota_tracker import ota_tracker

router = APIRouter()

UPLOAD_DIR = Path("uploads/firmwares")
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)


def _build_download_url(request: Request, firmware_id: int) -> str:
    return build_signed_download_url(
        request,
        f"/api/v2/firmwares/{firmware_id}/download",
        f"fw:{firmware_id}",
    )


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

    orig_name = Path(file.filename or "").name
    if not orig_name or not orig_name.endswith(".bin"):
        raise HTTPException(status_code=400, detail="Only .bin firmware files are supported")

    file_uuid = str(uuid.uuid4())[:8]
    safe_filename = f"{file_uuid}_{orig_name}"
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
        filename=orig_name,
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
    sig: Optional[str] = Query(None),
    db: Session = Depends(get_db),
    user: Optional[User] = Depends(get_optional_user),
):
    require_download_auth(f"fw:{firmware_id}", sig, user)
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

        ud = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == dev_uuid).first()
        if ud and ud.chip:
            ok_chip, chip_err = check_chip_compatibility(ud.chip, fw.target_chip or "")
            if not ok_chip:
                results.append({"device_uuid": dev_uuid, "status": "failed", "error": chip_err})
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
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    ensure_device_access(db, user, device_uuid)
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
    ensure_device_access(db, user, device_uuid)

    res = await dispatcher.deliver_command(
        db,
        device_uuid=device_uuid,
        cmd_type="ota.status",
        payload={},
        user_id=user.id,
        timeout_ms=3000,
    )
    return res
