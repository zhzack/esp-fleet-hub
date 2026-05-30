"""多目标动作编排：依次切换到每个目标 BLE peer，等待连接，然后下发 action。

核心 API：`send_to_targets(...)`，对每个 target：
    1. 查 `ble.info` → 已经是这个 peer? 直接发
    2. 否则下 `ble.switch` → 轮询 `ble.info` 等到 connected==target（最多 N 秒）
    3. 下 `action` envelope，等 ACK，记录结果
"""
from __future__ import annotations

import asyncio
from dataclasses import dataclass
from typing import Any

from sqlalchemy.orm import Session

from app.core.logger.logger import logger
from app.services import dispatcher


@dataclass
class Target:
    addr_type: int
    addr: str                     # "AA:BB:CC:DD:EE:FF"
    slot_index: int | None = None
    name: str | None = None


@dataclass
class TargetResult:
    addr: str
    addr_type: int
    slot_index: int | None
    name: str | None
    switched: bool
    connected: bool
    status: str                   # "acked" | "failed" | "timeout" | "skipped"
    exec_time_ms: int = 0
    error_code: str | None = None
    error_message: str | None = None
    msg_id: str | None = None


def _normalize_addr(s: str) -> str:
    return s.upper().replace("-", ":").replace(" ", "")


async def _ble_info(db: Session, user_id: int, device_uuid: str) -> dict | None:
    """读取设备当前 BLE 状态。返回 result 字段（{connected, peer, ...}）或 None。"""
    resp = await dispatcher.deliver_command(
        db,
        device_uuid=device_uuid,
        cmd_type="ble.info",
        payload={},
        user_id=user_id,
        timeout_ms=2000,
    )
    if resp.get("status") != "acked":
        return None
    ack_payload = resp.get("result") or {}
    inner = ack_payload.get("result") or {}
    return inner


async def _ble_switch(db: Session, user_id: int, device_uuid: str, t: Target) -> bool:
    addr_bytes = [int(x, 16) for x in t.addr.split(":")]
    if len(addr_bytes) != 6:
        return False
    resp = await dispatcher.deliver_command(
        db,
        device_uuid=device_uuid,
        cmd_type="ble.switch",
        payload={"addr_type": t.addr_type, "addr": addr_bytes},
        user_id=user_id,
        timeout_ms=3000,
    )
    return resp.get("status") == "acked"


async def _wait_connected_to(
    db: Session,
    user_id: int,
    device_uuid: str,
    t: Target,
    *,
    timeout_s: float = 5.0,
    poll_ms: int = 250,
) -> bool:
    """轮询 ble.info 直到当前 peer 等于 target。"""
    end = asyncio.get_event_loop().time() + timeout_s
    target_addr = _normalize_addr(t.addr)
    while asyncio.get_event_loop().time() < end:
        info = await _ble_info(db, user_id, device_uuid)
        if info and info.get("connected"):
            peer_addr = _normalize_addr(str(info.get("peer", "")))
            if peer_addr == target_addr:
                return True
        await asyncio.sleep(poll_ms / 1000)
    return False


async def _send_action(
    db: Session,
    user_id: int,
    device_uuid: str,
    action: dict,
    timeout_ms: int,
) -> dict:
    """把 action 当 command 直接下发（走 cmd_router 同步路径），拿到设备 ACK 整包。"""
    return await dispatcher.deliver_command(
        db,
        device_uuid=device_uuid,
        cmd_type="action",
        payload=action,
        user_id=user_id,
        timeout_ms=timeout_ms,
    )


async def send_to_targets(
    db: Session,
    *,
    user_id: int,
    device_uuid: str,
    targets: list[Target],
    action: dict,
    require_connected: bool = True,
    switch_timeout_s: float = 5.0,
    action_timeout_ms: int = 4000,
) -> list[TargetResult]:
    """依次对每个 target 发同一个 action，返回每个目标的执行结果。"""
    results: list[TargetResult] = []
    if not targets:
        return results

    for t in targets:
        target_addr = _normalize_addr(t.addr)
        switched = False
        connected = False

        # 1) 当前 peer 检查
        info = await _ble_info(db, user_id, device_uuid)
        cur_addr = _normalize_addr(str((info or {}).get("peer", "")))
        cur_connected = bool((info or {}).get("connected"))

        # 2) 不一致则切换
        if not cur_connected or cur_addr != target_addr:
            ok = await _ble_switch(db, user_id, device_uuid, t)
            if not ok:
                results.append(TargetResult(
                    addr=t.addr, addr_type=t.addr_type,
                    slot_index=t.slot_index, name=t.name,
                    switched=False, connected=False,
                    status="failed",
                    error_code="E101",
                    error_message="ble.switch failed",
                ))
                continue
            switched = True
            # 等连上
            connected = await _wait_connected_to(
                db, user_id, device_uuid, t, timeout_s=switch_timeout_s,
            )
        else:
            connected = True

        if not connected and require_connected:
            results.append(TargetResult(
                addr=t.addr, addr_type=t.addr_type,
                slot_index=t.slot_index, name=t.name,
                switched=switched, connected=False,
                status="timeout",
                error_code="E102",
                error_message=f"peer {t.addr} not connected after switch",
            ))
            continue

        # 3) 发送 action
        try:
            resp = await _send_action(db, user_id, device_uuid, action, action_timeout_ms)
        except Exception as e:
            logger.exception(f"[orchestrator] send_action error: {e}")
            results.append(TargetResult(
                addr=t.addr, addr_type=t.addr_type,
                slot_index=t.slot_index, name=t.name,
                switched=switched, connected=connected,
                status="failed",
                error_code="E103",
                error_message=str(e),
            ))
            continue

        ack = (resp.get("result") or {})
        ack_ok = bool(ack.get("ok"))
        exec_ms = int(ack.get("exec_time_ms", resp.get("exec_time_ms", 0)) or 0)
        if resp.get("status") == "acked" and ack_ok:
            results.append(TargetResult(
                addr=t.addr, addr_type=t.addr_type,
                slot_index=t.slot_index, name=t.name,
                switched=switched, connected=connected,
                status="acked",
                exec_time_ms=exec_ms,
                msg_id=resp.get("msg_id"),
            ))
        else:
            results.append(TargetResult(
                addr=t.addr, addr_type=t.addr_type,
                slot_index=t.slot_index, name=t.name,
                switched=switched, connected=connected,
                status=resp.get("status", "failed"),
                exec_time_ms=exec_ms,
                error_code=ack.get("err_code") or resp.get("error_code"),
                error_message=ack.get("err_msg") or resp.get("error_message"),
                msg_id=resp.get("msg_id"),
            ))

    return results
