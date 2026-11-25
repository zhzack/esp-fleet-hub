"""Device-facing /api/fleet/* compatibility routes inside py_web FastAPI."""

from pathlib import Path
import re
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from fastapi.responses import FileResponse, JSONResponse

from app.core.config.config import settings
from app.core.security import get_optional_user
from app.database.models.user import User
from app.services import unified_bridge as ub
from app.services.firmware_utils import require_download_auth

router = APIRouter()

_DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


def _validate_device_id(dev_id: str) -> str:
    clean = str(dev_id or "").strip()
    if not _DEVICE_ID_RE.match(clean):
        raise HTTPException(400, "invalid device_id format")
    return clean


def _extract_proof(request: Request, payload: dict | None) -> tuple[str, str]:
    nonce = (request.headers.get("X-Device-Nonce") or str((payload or {}).get("nonce", ""))).strip()
    proof = (request.headers.get("X-Device-Proof") or str((payload or {}).get("proof", ""))).strip()
    return nonce, proof


def _verify_device_hmac_if_needed(
    request: Request,
    dev_id: str,
    payload: dict | None,
    *,
    require_when_known: bool = False,
) -> Optional[JSONResponse]:
    nonce, proof = _extract_proof(request, payload)
    existing = ub.hub_db.get_device(dev_id)
    has_secret = bool(existing and existing.get("has_secret")) or bool(settings.DEVICE_SHARED_SECRET)
    must_verify = bool(proof or nonce) or settings.FLEET_STRICT_DEVICE_AUTH or (require_when_known and has_secret)
    if not must_verify:
        return None
    if not nonce or not proof:
        return JSONResponse(status_code=401, content={"ok": False, "error": "HMAC challenge proof required"})
    ok, msg = ub.hub_db.verify_and_consume_hmac(
        dev_id,
        nonce,
        proof,
        global_secret=settings.DEVICE_SHARED_SECRET,
    )
    if not ok:
        ub.hub_db.log_audit(dev_id, "auth_failed", msg)
        return JSONResponse(status_code=401, content={"ok": False, "error": msg})
    return None


@router.get("/challenge")
def fleet_challenge(device_id: str = Query("")):
    clean_id = _validate_device_id(device_id) if device_id else ""
    nonce = ub.hub_db.issue_nonce(clean_id)
    return {"ok": True, "nonce": nonce}


@router.post("/register")
async def fleet_register(request: Request):
    payload = await request.json()
    if not isinstance(payload, dict):
        return JSONResponse(status_code=400, content={"ok": False, "error": "invalid JSON object"})
    dev_id = str(payload.get("device_id", "")).strip()
    if not dev_id:
        return JSONResponse(status_code=400, content={"ok": False, "error": "missing device_id"})
    if not _DEVICE_ID_RE.match(dev_id):
        return JSONResponse(status_code=400, content={"ok": False, "error": "invalid device_id format"})

    err_resp = _verify_device_hmac_if_needed(request, dev_id, payload, require_when_known=True)
    if err_resp is not None:
        return err_resp

    dev = ub.hub_db.register_or_update_device(payload)
    return JSONResponse(status_code=200, content={"ok": True, "device": dev})


@router.post("/heartbeat")
async def fleet_heartbeat(request: Request):
    payload = await request.json()
    if not isinstance(payload, dict):
        return JSONResponse(status_code=400, content={"ok": False, "error": "invalid JSON object"})
    dev_id = str(payload.get("device_id", "")).strip()
    if not dev_id:
        return JSONResponse(status_code=400, content={"ok": False, "error": "missing device_id"})
    if not _DEVICE_ID_RE.match(dev_id):
        return JSONResponse(status_code=400, content={"ok": False, "error": "invalid device_id format"})

    err_resp = _verify_device_hmac_if_needed(request, dev_id, payload, require_when_known=False)
    if err_resp is not None:
        return err_resp

    return ub.hub_db.process_heartbeat(dev_id, payload)


@router.post("/cmd_ack")
async def fleet_cmd_ack(request: Request):
    payload = await request.json()
    if not isinstance(payload, dict):
        return JSONResponse(status_code=400, content={"ok": False, "error": "invalid JSON object"})
    dev_id = str(payload.get("device_id", "")).strip()
    if not _DEVICE_ID_RE.match(dev_id):
        return JSONResponse(status_code=400, content={"ok": False, "error": "invalid device_id format"})
    if not ub.hub_db.get_device(dev_id):
        return JSONResponse(status_code=404, content={"ok": False, "error": "unknown device"})

    err_resp = _verify_device_hmac_if_needed(request, dev_id, payload, require_when_known=False)
    if err_resp is not None:
        return err_resp

    cmd_id = payload.get("cmd_id")
    ok = bool(payload.get("ok", True))
    ub.hub_db.log_audit(dev_id, "cmd_ack", f"cmd_id={cmd_id} ok={ok}")
    return {"ok": True}


@router.get("/download")
def fleet_firmware_download(
    file: str = Query(...),
    sig: Optional[str] = Query(None),
    user: Optional[User] = Depends(get_optional_user),
):
    safe = Path(file).name
    require_download_auth(f"fleet:{safe}", sig, user)
    fp = Path(ub.build_flavor.DIST_DIR) / safe
    if not fp.is_file():
        raise HTTPException(404, f"Firmware not found: {safe}")
    return FileResponse(
        path=fp,
        media_type="application/octet-stream",
        filename=safe,
    )
