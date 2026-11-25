"""Authenticated /api/v2/unified/* management & cloud compiler routes for Vue SPA."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Optional
import urllib.parse

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from fastapi.responses import FileResponse, JSONResponse, Response
from pydantic import BaseModel
from sqlalchemy.orm import Session

from app.core.security import get_current_admin, get_current_user
from app.database.crud import device as crud_device
from app.database.models.device import Device
from app.database.models.user import User
from app.database.schemas.firmware import FirmwareOut
from app.database.session import get_db
from app.services import unified_bridge as ub

router = APIRouter()


class ClaimIn(BaseModel):
    name: Optional[str] = None
    claimed: bool = True
    api_token: Optional[str] = None


class ConfigPushIn(BaseModel):
    config: Dict[str, Any]


class CommandIn(BaseModel):
    cmd_type: str
    payload: Dict[str, Any] = {}


class BatchOtaIn(BaseModel):
    file: str
    device_ids: List[str]


class PublishIn(BaseModel):
    version: Optional[str] = None
    description: Optional[str] = None


def _request_port(request: Request) -> int:
    if request.url.port:
        return int(request.url.port)
    return 8000


def _require_device_access(
    db: Session,
    user: User,
    device_id: str,
    *,
    allow_unclaimed: bool = False,
) -> Dict[str, Any]:
    """Enforce admin or owning-user authorization on a unified device."""
    ub.hub_db.ensure_init()
    hub_dev = ub.hub_db.get_device(device_id)
    if not hub_dev:
        raise HTTPException(404, "Device not found")
    if user.role == "admin":
        return hub_dev

    app_dev = crud_device.get_by_uuid(db, device_id)
    if app_dev:
        if app_dev.owner_user_id == user.id:
            return hub_dev
        if app_dev.owner_user_id is None and allow_unclaimed and not hub_dev.get("claimed"):
            return hub_dev
        raise HTTPException(403, "无权操作其他用户的设备")

    if not hub_dev.get("claimed") and allow_unclaimed:
        return hub_dev
    raise HTTPException(403, "请先认领设备或使用管理员账号操作")


@router.get("/devices")
def list_unified_devices(
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    ub.hub_db.ensure_init()
    all_devs = ub.hub_db.list_devices()
    logs = ub.hub_db.list_audit_logs(40)
    if user.role == "admin":
        return {"ok": True, "devices": all_devs, "audit": logs}

    owners = {
        row[0]: row[1]
        for row in db.query(Device.device_uuid, Device.owner_user_id).all()
    }
    visible = [
        d for d in all_devs
        if owners.get(d["device_id"]) == user.id
        or (owners.get(d["device_id"]) is None and not d.get("claimed"))
    ]
    visible_ids = {d["device_id"] for d in visible}
    return {
        "ok": True,
        "devices": visible,
        "audit": [a for a in logs if a.get("device_id") in visible_ids],
    }


@router.post("/devices/{device_id}/claim")
def claim_unified_device(
    device_id: str,
    body: ClaimIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _require_device_access(db, user, device_id, allow_unclaimed=True)
    dev = ub.hub_db.update_device_meta(
        device_id,
        name=body.name,
        claimed=body.claimed,
        api_token=body.api_token,
    )
    if not dev:
        raise HTTPException(404, "Device not found")

    app_dev = crud_device.get_by_uuid(db, device_id)
    target_owner = user.id if body.claimed else None
    if app_dev is None:
        crud_device.create(
            db,
            device_uuid=device_id,
            owner_user_id=target_owner,
            name=dev.get("name") or device_id,
            capabilities=[],
        )
    else:
        app_dev.name = dev.get("name") or app_dev.name
        if body.claimed and app_dev.owner_user_id is None:
            app_dev.owner_user_id = user.id
        elif not body.claimed and (user.role == "admin" or app_dev.owner_user_id == user.id):
            app_dev.owner_user_id = None
        db.commit()

    ub.hub_db.log_audit(device_id, "claimed", f"name={dev.get('name')} by={user.username}")
    return {"ok": True, "device": dev}


@router.post("/devices/{device_id}/config")
def push_unified_config(
    device_id: str,
    body: ConfigPushIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _require_device_access(db, user, device_id, allow_unclaimed=False)
    code, res = ub.fleet_router.push_config_to_device(device_id, body.config)
    return JSONResponse(status_code=code, content=res)


@router.post("/devices/{device_id}/command")
def send_unified_command(
    device_id: str,
    body: CommandIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _require_device_access(db, user, device_id, allow_unclaimed=False)
    code, res = ub.fleet_router.send_command_to_device(device_id, body.cmd_type, body.payload)
    return JSONResponse(status_code=code, content=res)


@router.delete("/devices/{device_id}")
def delete_unified_device(
    device_id: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    _require_device_access(db, user, device_id, allow_unclaimed=False)
    ok = ub.hub_db.delete_device(device_id)
    app_dev = crud_device.get_by_uuid(db, device_id)
    if app_dev:
        db.delete(app_dev)
        db.commit()
    return {"ok": ok}


@router.get("/devices/{device_id}/proxy")
def proxy_unified_get(
    device_id: str,
    path: str = Query("/api/system/status"),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    dev = _require_device_access(db, user, device_id, allow_unclaimed=False)
    target = path if path.startswith("/") else "/" + path
    ok, status, result = ub.fleet_router._direct_device_http(dev, target, method="GET", timeout=8)
    if not ok:
        raise HTTPException(502, f"Proxy failed ({status}): {result}")
    raw, ctype = result
    return Response(content=raw, status_code=status, media_type=ctype)


@router.post("/devices/{device_id}/proxy")
async def proxy_unified_post(
    device_id: str,
    request: Request,
    path: str = Query(...),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    dev = _require_device_access(db, user, device_id, allow_unclaimed=False)
    try:
        payload = await request.json()
    except Exception:
        payload = {}
    target = path if path.startswith("/") else "/" + path
    ok, status, result = ub.fleet_router._direct_device_http(
        dev, target, method="POST", payload=payload, timeout=8
    )
    if not ok:
        raise HTTPException(502, f"Proxy failed ({status}): {result}")
    raw, ctype = result
    return Response(content=raw, status_code=status, media_type=ctype)


@router.get("/profiles")
def get_profiles(_user: User = Depends(get_current_user)):
    return {"ok": True, "profiles": ub.web_compiler.list_profiles_data()}


@router.get("/ports")
def get_ports(_admin: User = Depends(get_current_admin)):
    return {"ok": True, "ports": ub.web_compiler.list_serial_ports()}


@router.get("/firmwares")
def get_unified_firmwares(_user: User = Depends(get_current_user)):
    return {"ok": True, "firmwares": ub.fleet_router.list_firmware_items()}


@router.get("/firmwares/{file_name}/download")
def download_unified_firmware(
    file_name: str,
    _user: User = Depends(get_current_user),
):
    safe = Path(file_name).name
    fp = Path(ub.build_flavor.DIST_DIR) / safe
    if not fp.is_file():
        raise HTTPException(404, "Firmware not found")
    return FileResponse(path=fp, media_type="application/octet-stream", filename=safe)


@router.post("/firmwares/{file_name}/publish", response_model=FirmwareOut)
def publish_unified_firmware(
    file_name: str,
    body: PublishIn = PublishIn(),
    db: Session = Depends(get_db),
    _admin: User = Depends(get_current_admin),
):
    safe = Path(file_name).name
    try:
        return ub.publish_firmware_to_hid_repo(
            db, safe, version_override=body.version, description=body.description
        )
    except FileNotFoundError as e:
        raise HTTPException(404, str(e))


@router.post("/ota/batch")
def batch_unified_ota(
    body: BatchOtaIn,
    request: Request,
    _admin: User = Depends(get_current_admin),
):
    ub.hub_db.ensure_init()
    port = _request_port(request)
    code, res = ub.fleet_router.trigger_batch_ota(
        body.file, body.device_ids, server_port=port, dl_path="/api/fleet/download"
    )
    return JSONResponse(status_code=code, content=res)


@router.post("/build")
async def start_unified_build(
    request: Request,
    _admin: User = Depends(get_current_admin),
):
    cur = ub.web_compiler.get_task_snapshot()
    if cur.get("status") in ("building", "flashing"):
        raise HTTPException(409, f"已有任务正在执行中 ({cur.get('status')})，请稍候")
    payload = await request.json()
    try:
        ub.start_cloud_build(payload, hub_port=_request_port(request))
    except ValueError as e:
        raise HTTPException(400, str(e))
    return {"ok": True, "message": "Build task launched"}


@router.get("/task")
def get_compiler_task(_user: User = Depends(get_current_user)):
    return ub.web_compiler.get_task_snapshot()


@router.post("/flash_serial")
async def start_unified_serial_flash(
    request: Request,
    _admin: User = Depends(get_current_admin),
):
    cur = ub.web_compiler.get_task_snapshot()
    if cur.get("status") in ("building", "flashing"):
        raise HTTPException(409, f"已有任务正在执行中 ({cur.get('status')})，请稍候")
    ub.hub_db.ensure_init()
    payload = await request.json()
    ub.start_serial_flash(payload, hub_port=_request_port(request))
    return {"ok": True, "message": "Serial flash started"}
