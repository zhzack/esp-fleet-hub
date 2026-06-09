from pathlib import Path
from uuid import uuid4

from fastapi import APIRouter, Depends, HTTPException, WebSocket

from app.core.logger.logger import logger
from app.core.security import get_current_user, ws_authenticate
from app.database.models.user import User
from app.database.session import SessionLocal
from app.services.log_bus import log_bus
from app.services.script_config import ScriptConfig
from app.services.script_runner import run_script_async

router = APIRouter()

SCRIPTS_DIR = Path(__file__).resolve().parents[2] / "resource" / "scripts"
logger.info(f"SCRIPTS_DIR: {SCRIPTS_DIR}")


@router.get("/list")
def list_scripts(_user: User = Depends(get_current_user)):
    return {"scripts": [f.name for f in SCRIPTS_DIR.glob("*") if f.is_file()]}


@router.get("/configs")
def list_configs(script: str, _user: User = Depends(get_current_user)):
    cfg_dir = SCRIPTS_DIR / script
    if not cfg_dir.exists():
        return {"configs": []}
    return {"configs": [f.name for f in cfg_dir.glob("*.yaml")]}


@router.get("/params")
def get_params(script: str, config: str, _user: User = Depends(get_current_user)):
    return {"params": [{"name": "count", "default": 5}, {"name": "delay", "default": 1}]}


@router.post("/run")
def run_script(
    script: str,
    config_name: str,
    args: list[str] | None = None,
    _user: User = Depends(get_current_user),
):
    _ = config_name
    script_path = SCRIPTS_DIR / script
    if not script_path.exists():
        raise HTTPException(404, "Script not found")

    config = ScriptConfig(
        script_path=script_path,
        workdir=script_path.parent,
        interpreter=None,
        args=args or [],
        env={},
    )

    execution_id = str(uuid4())
    run_script_async(config, execution_id)
    return {"execution_id": execution_id}


@router.websocket("/ws/{execution_id}")
async def ws_logs(ws: WebSocket, execution_id: str):
    db = SessionLocal()
    try:
        auth_user = await ws_authenticate(ws, db)
        if auth_user is None:
            return
        await ws.accept()
        async for line in log_bus.subscribe(execution_id):
            await ws.send_text(line)
    finally:
        db.close()
