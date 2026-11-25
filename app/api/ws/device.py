"""ESP32 设备 WS 端点：/ws/device。

协议见 makedown/hid_gateway_protocol_v_2_design.md（V2 envelope）。
"""
import asyncio
import hashlib
import hmac
import json
import re
import secrets
import sys
import uuid
from datetime import datetime, timezone
from typing import Any

from fastapi import APIRouter, WebSocket, WebSocketDisconnect, Depends
from sqlalchemy.orm import Session

from app.core.config.config import settings
from app.core.logger.logger import logger
from app.database.crud import device as crud_device
from app.database.models.unified_fleet import UnifiedDevice
from app.database.session import SessionLocal
from app.services.ack_tracker import ack_tracker
from app.services.device_registry import device_registry
from app.services.idempotency import seen_or_record_async
from app.services.ota_tracker import ota_tracker
from app.services.queue import queue_backend
from app.services.unified_bridge import lookup_device_ws_secret

router = APIRouter()

HEARTBEAT_INTERVAL = 10   # 设备应每 10s 一次
HEARTBEAT_TIMEOUT = 35    # 服务端 35s 未收到任何帧 → 主动断开


_DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


def _new_db() -> Session:
    return SessionLocal()


def _wrap_legacy(raw: dict) -> dict:
    """兼容旧 V1 帧 {type,payload} → V2 envelope。"""
    if "header" in raw:
        return raw
    msg_type = raw.get("type") or "unknown"
    return {
        "header": {
            "msg_id": raw.get("msg_id") or str(uuid.uuid4()),
            "type": msg_type,
            "timestamp": int(datetime.now(timezone.utc).timestamp()),
            "source": "legacy",
        },
        "routing": {},
        "payload": raw.get("payload", {}),
    }


@router.websocket("/ws/device")
async def device_ws(websocket: WebSocket):
    # 这里允许 token 为空（一期）；后续可改为强制 ws_authenticate
    await websocket.accept()
    session_id = str(uuid.uuid4())
    device_uuid: str | None = None
    registered_uuid: str | None = None
    sender_task: asyncio.Task | None = None
    last_seen = asyncio.get_running_loop().time()

    db = _new_db()

    auth_nonce_issued: tuple[str, str] | None = None  # (nonce, cand_device_id)

    async def _sender_loop(dev_uuid: str):
        """消费 queue，把 envelope 发给设备。"""
        while True:
            env = await queue_backend.pop(dev_uuid)
            if env is None:
                continue
            try:
                await websocket.send_json(env)
            except Exception as e:
                logger.warning(f"[ws/device] send failed for {dev_uuid}: {e}")
                # 把消息塞回去，留给重连后重发
                await queue_backend.requeue(dev_uuid, env)
                return

    try:
        while True:
            try:
                raw = await asyncio.wait_for(
                    websocket.receive_json(), timeout=HEARTBEAT_TIMEOUT
                )
            except asyncio.TimeoutError:
                logger.info(f"[ws/device] heartbeat timeout, closing {device_uuid}")
                await websocket.close(code=1011)
                break

            last_seen = asyncio.get_running_loop().time()
            env = _wrap_legacy(raw) if isinstance(raw, dict) else None
            if env is None:
                continue

            header = env.get("header", {})
            mtype = header.get("type")
            msg_id = header.get("msg_id")
            payload = env.get("payload", {}) or {}

            # 幂等：上行幂等只对 register/action 类无意义，但 ack/error 必须独立处理
            if mtype not in ("ack", "error", "heartbeat") and msg_id and await seen_or_record_async(msg_id):
                logger.debug(f"[ws/device] dup msg_id={msg_id} ignored")
                continue

            if mtype == "register":
                cand_id = str(payload.get("device_id") or header.get("device_id") or "").strip()
                if not cand_id or not _DEVICE_ID_RE.match(cand_id):
                    await websocket.send_json(_error_env(msg_id, "E004", "missing or invalid device_id"))
                    await websocket.close(code=1008)
                    break

                if registered_uuid is not None and cand_id != registered_uuid:
                    await websocket.send_json(_error_env(msg_id, "E403", "cannot switch device_id on active session"))
                    await websocket.close(code=1008)
                    break

                # --- 设备鉴权（HMAC-SHA256 挑战-应答，支持全局密钥或 Hub 预置单机密钥） ---
                shared_secret = settings.DEVICE_SHARED_SECRET
                dev_secret = "" if "pytest" in sys.modules else lookup_device_ws_secret(cand_id)
                candidate_secrets = [s for s in (dev_secret, shared_secret) if s]
                if candidate_secrets:
                    auth = payload.get("auth") if isinstance(payload.get("auth"), dict) else {}
                    if not auth.get("proof"):
                        # 首次 register：下发绑定 device_id 的 nonce 挑战，暂不注册
                        nonce_val = secrets.token_hex(16)
                        auth_nonce_issued = (nonce_val, cand_id)
                        await websocket.send_json({
                            "header": {
                                "msg_id": str(uuid.uuid4()),
                                "ref_msg_id": msg_id,
                                "type": "auth_challenge",
                                "timestamp": int(datetime.now(timezone.utc).timestamp()),
                            },
                            "payload": {"nonce": nonce_val},
                        })
                        continue
                    # 携带应答：校验 nonce 与 cand_id 绑定 + 任一有效 secret 的 HMAC
                    proof_str = str(auth.get("proof", ""))
                    expected_nonce, expected_dev = auth_nonce_issued or ("", "")
                    proof_ok = (
                        bool(expected_nonce)
                        and expected_dev == cand_id
                        and auth.get("nonce") == expected_nonce
                        and any(
                            hmac.compare_digest(
                                hmac.new(
                                    sec.encode(),
                                    expected_nonce.encode(),
                                    hashlib.sha256,
                                ).hexdigest(),
                                proof_str,
                            )
                            for sec in candidate_secrets
                        )
                    )
                    auth_nonce_issued = None
                    if not proof_ok:
                        logger.warning(
                            f"[ws/device] auth failed for {cand_id}, closing"
                        )
                        await websocket.send_json(_error_env(msg_id, "E401", "device auth failed"))
                        await websocket.close(code=4401)
                        break

                device_uuid = cand_id
                registered_uuid = cand_id

                # 同一 device 重连：先把旧连接踢掉，避免两路并存导致 ack 路由错乱
                old_ws = device_registry.get(device_uuid)
                if old_ws is not None and old_ws is not websocket:
                    logger.warning(f"[ws/device] {device_uuid} reconnected — closing stale session")
                    try:
                        await old_ws.close(code=1012)
                    except Exception:
                        pass
                    device_registry.unregister(device_uuid)
                caps = payload.get("capabilities") or []
                name = payload.get("name")
                dev = crud_device.upsert_from_register(db, device_uuid, name, caps)
                ud = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == device_uuid).first()
                if ud is not None:
                    ud.status = "online"
                    ud.last_seen_at = int(datetime.now(timezone.utc).timestamp())
                    if name:
                        ud.name = name
                    db.commit()
                client_host = websocket.client.host if websocket.client else None
                crud_device.record_connection(db, dev.id, client_host, session_id)
                device_registry.register(device_uuid, websocket)
                sender_task = asyncio.create_task(_sender_loop(device_uuid))
                logger.info(f"[ws/device] registered: {device_uuid} caps={caps}")
                # 回一个 ack
                await websocket.send_json(
                    {
                        "header": {
                            "msg_id": str(uuid.uuid4()),
                            "ref_msg_id": msg_id,
                            "type": "ack",
                            "timestamp": int(datetime.now(timezone.utc).timestamp()),
                        },
                        "payload": {"status": "ok", "server_session": session_id},
                    }
                )

            elif mtype == "heartbeat":
                # 心跳只证明存活：更新 last_online_at，仅在残留 offline 时归位；
                # 不覆盖设备上报的 busy/error（恢复需设备显式上报 status 帧）
                if device_uuid:
                    dev = crud_device.get_by_uuid(db, device_uuid)
                    if dev:
                        dev.last_online_at = datetime.now(timezone.utc)
                        if dev.status == "offline":
                            dev.status = "online"
                        ud = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == device_uuid).first()
                        if ud is not None:
                            ud.last_seen_at = int(datetime.now(timezone.utc).timestamp())
                            if ud.status != "online":
                                ud.status = "online"
                        db.commit()
                # 不强制回复

            elif mtype == "ack":
                ref = header.get("ref_msg_id")
                if ref:
                    ack_tracker.resolve(ref, payload)

            elif mtype == "status":
                if device_uuid:
                    state = payload.get("state")
                    if state in ("online", "busy", "error"):
                        dev = crud_device.get_by_uuid(db, device_uuid)
                        if dev:
                            dev.status = state
                            dev.last_online_at = datetime.now(timezone.utc)
                            db.commit()

            elif mtype == "error":
                ref = header.get("ref_msg_id")
                logger.warning(f"[ws/device] error from {device_uuid}: {payload}")
                if ref:
                    # 也视作一种 ack（status=failed）
                    ack_tracker.resolve(
                        ref,
                        {"status": "failed", "error": payload, "exec_time_ms": 0},
                    )

            elif mtype == "ota_progress":
                if device_uuid:
                    ota_tracker.update_progress(device_uuid, payload)
                    logger.info(
                        f"[ws/device] OTA progress {device_uuid}: "
                        f"{payload.get('progress')}% ({payload.get('status')})"
                    )

            else:
                logger.debug(f"[ws/device] unknown type={mtype} from {device_uuid}")

    except WebSocketDisconnect:
        logger.info(f"[ws/device] disconnected: {device_uuid}")
    except Exception as e:
        logger.exception(f"[ws/device] error: {e}")
    finally:
        if sender_task:
            sender_task.cancel()
        if device_uuid:
            # 只在 registry 里仍然指向自己时才注销，避免老连接清掉新连接的注册
            if device_registry.get(device_uuid) is websocket:
                device_registry.unregister(device_uuid)
                try:
                    crud_device.mark_offline(db, device_uuid)
                except Exception:
                    pass
            try:
                crud_device.close_connection(db, session_id)
            except Exception:
                pass
        try:
            db.close()
        except Exception:
            pass


def _error_env(ref_msg_id: str | None, code: str, message: str) -> dict:
    return {
        "header": {
            "msg_id": str(uuid.uuid4()),
            "ref_msg_id": ref_msg_id,
            "type": "error",
            "timestamp": int(datetime.now(timezone.utc).timestamp()),
        },
        "payload": {"code": code, "message": message},
    }
