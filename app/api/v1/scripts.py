from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Optional
from uuid import uuid4

from fastapi import APIRouter, Depends, HTTPException, WebSocket

from app.core.logger.logger import logger
from app.core.security import get_current_admin, get_current_user, ws_authenticate
from app.database.models.user import User
from app.database.session import SessionLocal
from app.services.log_bus import log_bus
from app.services.script_config import ScriptConfig
from app.services.script_runner import run_script_async

router = APIRouter()

SCRIPTS_DIR = Path(__file__).resolve().parents[2] / "resource" / "scripts"
logger.info(f"SCRIPTS_DIR: {SCRIPTS_DIR}")

_ALLOWED_SCRIPT_SUFFIXES = {".py", ".sh", ".bat", ".cmd"}


def _resolve_safe_path(base_dir: Path, untrusted_name: str, *, allow_dir: bool = False) -> Path:
    """Resolve user-supplied relative name strictly inside `base_dir`, blocking traversal."""
    if not untrusted_name or any(sep in untrusted_name for sep in ("/", "\\", "\x00")) or untrusted_name in (".", ".."):
        raise HTTPException(400, "Invalid script path")
    root = base_dir.resolve()
    candidate = (root / untrusted_name).resolve()
    if not candidate.is_relative_to(root) or candidate == root:
        raise HTTPException(400, "Path escapes allowed directory")
    if not allow_dir and candidate.is_dir():
        raise HTTPException(400, "Expected file path")
    return candidate


@router.get("/list")
def list_scripts(_user: User = Depends(get_current_user)):
    if not SCRIPTS_DIR.exists():
        return {"scripts": []}
    return {"scripts": sorted(f.name for f in SCRIPTS_DIR.iterdir() if f.is_file())}


@router.get("/configs")
def list_configs(script: str, _user: User = Depends(get_current_user)):
    cfg_dir = _resolve_safe_path(SCRIPTS_DIR, script, allow_dir=True)
    if not cfg_dir.is_dir():
        return {"configs": []}
    return {"configs": sorted(f.name for f in cfg_dir.glob("*.yaml"))}


@router.get("/params")
def get_params(script: str, config: str, _user: User = Depends(get_current_user)):
    _resolve_safe_path(SCRIPTS_DIR, script, allow_dir=True)
    return {"params": [{"name": "count", "default": 5}, {"name": "delay", "default": 1}]}


@router.post("/run")
async def run_script(
    script: str,
    config_name: str,
    args: Optional[list[str]] = None,
    _admin: User = Depends(get_current_admin),
):
    _ = config_name
    script_path = _resolve_safe_path(SCRIPTS_DIR, script, allow_dir=False)
    if not script_path.is_file():
        raise HTTPException(404, "Script not found")
    if script_path.suffix.lower() not in _ALLOWED_SCRIPT_SUFFIXES:
        raise HTTPException(400, "Unsupported script extension")

    clean_args = [str(a) for a in (args or []) if len(str(a)) <= 256]
    config = ScriptConfig(
        script_path=script_path,
        workdir=SCRIPTS_DIR.resolve(),
        interpreter=None,
        args=clean_args,
        env={},
    )

    execution_id = str(uuid4())
    run_script_async(config, execution_id, loop=asyncio.get_running_loop())
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
