"""Device-facing /api/fleet/* compatibility routes inside py_web FastAPI."""

from pathlib import Path

from fastapi import APIRouter, HTTPException, Query, Request
from fastapi.responses import FileResponse, JSONResponse

from app.services import unified_bridge as ub

router = APIRouter()


@router.get("/challenge")
def fleet_challenge(device_id: str = Query("")):
    nonce = ub.hub_db.issue_nonce(device_id)
    return {"ok": True, "nonce": nonce}


@router.post("/register")
async def fleet_register(request: Request):
    payload = await request.json()
    code, body = ub.fleet_router.register_device_payload(payload or {})
    return JSONResponse(status_code=code, content=body)


@router.post("/heartbeat")
async def fleet_heartbeat(request: Request):
    payload = await request.json()
    dev_id = str((payload or {}).get("device_id", "")).strip()
    if not dev_id:
        return JSONResponse(status_code=400, content={"ok": False, "error": "missing device_id"})
    return ub.hub_db.process_heartbeat(dev_id, payload)


@router.post("/cmd_ack")
async def fleet_cmd_ack(request: Request):
    payload = await request.json()
    dev_id = str((payload or {}).get("device_id", ""))
    cmd_id = (payload or {}).get("cmd_id")
    ok = bool((payload or {}).get("ok", True))
    ub.hub_db.log_audit(dev_id, "cmd_ack", f"cmd_id={cmd_id} ok={ok}")
    return {"ok": True}


@router.get("/download")
def fleet_firmware_download(file: str = Query(...)):
    safe = Path(file).name
    fp = Path(ub.build_flavor.DIST_DIR) / safe
    if not fp.is_file():
        raise HTTPException(404, f"Firmware not found: {safe}")
    return FileResponse(
        path=fp,
        media_type="application/octet-stream",
        filename=safe,
    )
