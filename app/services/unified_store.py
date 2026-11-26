"""SQLAlchemy-backed Unified Fleet Store sharing py_web's primary database."""

from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import hmac
import json
import os
import secrets
import sqlite3
import sys
import time
from typing import Any

from sqlalchemy.orm import Session

import app.database.base  # noqa: F401 — ensure all ORM FK targets (e.g. users) are registered
from app.core.wifi_crypto import decrypt_field, encrypt_field
from app.database.crud import device as crud_device
from app.database.models.device import Device, DeviceConnection
from app.database.models.unified_fleet import (
    UnifiedAuditLog,
    UnifiedAuthNonce,
    UnifiedCompilerTask,
    UnifiedDevice,
    UnifiedDeviceConfig,
    UnifiedPendingCommand,
)
from app.database.session import SessionLocal, get_db


@contextmanager
def _session_ctx():
    """Yield an isolated Session, honoring FastAPI test dependency overrides."""
    main_mod = sys.modules.get("app.main")
    override = None
    if main_mod is not None and hasattr(main_mod, "app"):
        override = main_mod.app.dependency_overrides.get(get_db)
    if override is not None:
        gen = override()
        db = next(gen)
        try:
            yield db
        finally:
            try:
                gen.close()
            except Exception:
                pass
    else:
        db = SessionLocal()
        try:
            yield db
        finally:
            db.close()


def _row_to_device_dict(row: UnifiedDevice, cfg: UnifiedDeviceConfig | None, *, strip_secrets: bool) -> dict[str, Any]:
    d = {
        "id": row.id,
        "device_id": row.device_id,
        "mac": row.mac or "",
        "name": row.name or "",
        "chip": row.chip or "",
        "board": row.board or "",
        "flash_mb": row.flash_mb or 4,
        "psram_mb": row.psram_mb or 0,
        "ip": row.ip or "",
        "mdns_host": row.mdns_host or "",
        "web_port": row.web_port or 80,
        "fw_version": row.fw_version or "",
        "status": row.status or "provisioned",
        "claimed": row.claimed or 0,
        "rssi": row.rssi or 0,
        "free_heap": row.free_heap or 0,
        "uptime_sec": row.uptime_sec or 0,
        "last_seen_at": row.last_seen_at or 0,
        "created_at": row.created_at or 0,
    }
    try:
        d["capabilities"] = json.loads(row.capabilities_json or "{}")
    except Exception:
        d["capabilities"] = {}
    d["has_secret"] = bool(row.device_secret)
    if strip_secrets:
        d["has_api_token"] = bool(row.api_token)
    else:
        d["device_secret"] = decrypt_field(row.device_secret)
        d["api_token"] = decrypt_field(row.api_token)
    if cfg is not None:
        d["shadow"] = {
            "device_id": cfg.device_id,
            "desired_json": cfg.desired_json,
            "reported_json": cfg.reported_json,
            "desired_ver": cfg.desired_ver,
            "reported_ver": cfg.reported_ver,
            "sync_status": cfg.sync_status,
            "updated_at": cfg.updated_at,
        }
    else:
        d["shadow"] = {}
    return d


def _caps_dict_to_list(caps: Any) -> list[str]:
    if isinstance(caps, list):
        return [str(c).strip() for c in caps if str(c).strip()]
    if isinstance(caps, dict):
        return [str(k).strip() for k, v in caps.items() if isinstance(v, dict) and v.get("enabled")]
    return []


def _ensure_main_device(
    db: Session,
    device_id: str,
    name: str | None = None,
    claimed: int | None = None,
    capabilities: list[str] | None = None,
):
    """Keep py_web's primary `devices` table aligned with unified device metadata."""
    dev = crud_device.get_by_uuid(db, device_id)
    if dev is None:
        dev = Device(
            device_uuid=device_id,
            name=name or device_id,
            capabilities=capabilities or [],
        )
        db.add(dev)
    else:
        if name:
            dev.name = name
        if capabilities:
            dev.capabilities = capabilities
    return dev


class SqlAlchemyHubStore:
    """Thread-safe SQLAlchemy replacement for standalone sqlite hub_db."""

    def init_db(self, db_path=None):
        # Tables are managed by SQLAlchemy Base.metadata.create_all; custom file paths fall back to sqlite mode.
        pass

    def ensure_init(self):
        pass

    def close_db(self):
        pass

    def log_audit(self, device_id, event, detail=""):
        now = int(time.time())
        with _session_ctx() as db:
            db.add(
                UnifiedAuditLog(
                    device_id=device_id or "",
                    event=event,
                    detail=detail or "",
                    created_at=now,
                )
            )
            db.commit()

    def issue_nonce(self, device_id=""):
        nonce = secrets.token_hex(16)
        now = int(time.time())
        with _session_ctx() as db:
            db.query(UnifiedAuthNonce).filter(UnifiedAuthNonce.expires_at < now).delete()
            db.add(
                UnifiedAuthNonce(
                    nonce=nonce,
                    device_id=device_id or "",
                    expires_at=now + 120,
                )
            )
            db.commit()
        return nonce

    def verify_and_consume_hmac(self, device_id, nonce, proof_hex, global_secret=""):
        now = int(time.time())
        with _session_ctx() as db:
            row = db.query(UnifiedAuthNonce).filter(UnifiedAuthNonce.nonce == nonce).first()
            if not row or int(row.expires_at) < now:
                return False, "invalid or expired nonce"
            db.delete(row)
            dev = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == device_id).first()
            db.commit()
            dev_secret = decrypt_field(dev.device_secret) if dev and dev.device_secret else ""

        secret = dev_secret or global_secret
        if not secret:
            return True, "no_secret_configured"
        expected = hmac.new(
            secret.encode("utf-8"), nonce.encode("utf-8"), hashlib.sha256
        ).hexdigest()
        if hmac.compare_digest(expected, str(proof_hex or "").lower()):
            return True, "verified"
        return False, "hmac mismatch"

    def provision_device(
        self,
        device_id,
        mac="",
        chip="",
        flash_mb=4,
        board="",
        device_secret="",
        hub_url="",
        allow_empty_secret=False,
    ):
        now = int(time.time())
        if not device_secret and not allow_empty_secret:
            device_secret = secrets.token_hex(16)

        with _session_ctx() as db:
            existing = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == device_id).first()
            if existing:
                if mac:
                    existing.mac = mac
                if chip:
                    existing.chip = chip
                if flash_mb and int(flash_mb) > 0:
                    existing.flash_mb = int(flash_mb)
                if board:
                    existing.board = board
                if allow_empty_secret or device_secret:
                    existing.device_secret = encrypt_field(device_secret)
                if existing.status != "online":
                    existing.status = "provisioned"
            else:
                db.add(
                    UnifiedDevice(
                        device_id=device_id,
                        mac=mac or "",
                        name=device_id,
                        chip=chip or "",
                        board=board or "",
                        flash_mb=int(flash_mb or 4),
                        status="provisioned",
                        claimed=0,
                        device_secret=encrypt_field(device_secret),
                        created_at=now,
                    )
                )

            cfg = db.query(UnifiedDeviceConfig).filter(UnifiedDeviceConfig.device_id == device_id).first()
            initial_cfg = {"device_id": device_id}
            if hub_url:
                initial_cfg["hub_url"] = hub_url
            init_str = json.dumps(initial_cfg)
            if cfg is None:
                db.add(
                    UnifiedDeviceConfig(
                        device_id=device_id,
                        desired_json=init_str,
                        reported_json=init_str,
                        desired_ver=1,
                        reported_ver=1,
                        sync_status="synced",
                        updated_at=now,
                    )
                )
            else:
                # USB factory provisioning resets NVS config_ver to 1 and assigns fresh hub_url
                cfg.desired_json = init_str
                cfg.reported_json = init_str
                cfg.desired_ver = 1
                cfg.reported_ver = 1
                cfg.sync_status = "synced"
                cfg.updated_at = now

            _ensure_main_device(db, device_id, name=device_id)
            db.add(
                UnifiedAuditLog(
                    device_id=device_id,
                    event="provisioned",
                    detail=f"chip={chip} mac={mac} flash={flash_mb}MB",
                    created_at=now,
                )
            )
            db.commit()
        return self.get_device(device_id)

    def register_or_update_device(self, payload):
        now = int(time.time())
        device_id = str(payload.get("device_id", "")).strip()
        if not device_id:
            raise ValueError("device_id is required")

        mac = str(payload.get("mac", "")).upper()
        chip = str(payload.get("chip", ""))
        board = str(payload.get("board", ""))
        flash_mb = int(payload.get("flash_mb", 4) or 4)
        psram_bytes = int(payload.get("total_psram", 0) or payload.get("psram_bytes", 0) or 0)
        psram_mb = round(psram_bytes / (1024 * 1024)) if psram_bytes > 1024 else int(payload.get("psram_mb", 0) or 0)
        ip = str(payload.get("ip", ""))
        mdns_host = str(payload.get("mdns_host", ""))
        web_port = int(payload.get("web_port", 80) or 80)
        fw_version = str(payload.get("fw_version", ""))
        caps = payload.get("capabilities", {})
        caps_json = json.dumps(caps, ensure_ascii=False) if isinstance(caps, (dict, list)) else str(caps or "{}")

        reported_cfg = payload.get("config") or {}
        reported_ver = int(reported_cfg.get("config_ver", payload.get("config_ver", 1)) or 1)

        with _session_ctx() as db:
            dev = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == device_id).first()
            if dev:
                if mac:
                    dev.mac = mac
                if chip:
                    dev.chip = chip
                if board:
                    dev.board = board
                if flash_mb > 0:
                    dev.flash_mb = flash_mb
                dev.psram_mb = psram_mb
                if ip:
                    dev.ip = ip
                if mdns_host:
                    dev.mdns_host = mdns_host
                dev.web_port = web_port
                if fw_version:
                    dev.fw_version = fw_version
                dev.capabilities_json = caps_json
                dev.status = "online"
                dev.last_seen_at = now
            else:
                db.add(
                    UnifiedDevice(
                        device_id=device_id,
                        mac=mac,
                        name=device_id,
                        chip=chip,
                        board=board,
                        flash_mb=flash_mb,
                        psram_mb=psram_mb,
                        ip=ip,
                        mdns_host=mdns_host,
                        web_port=web_port,
                        fw_version=fw_version,
                        capabilities_json=caps_json,
                        status="online",
                        claimed=0,
                        last_seen_at=now,
                        created_at=now,
                    )
                )

            cfg_row = db.query(UnifiedDeviceConfig).filter(UnifiedDeviceConfig.device_id == device_id).first()
            rep_str = json.dumps(reported_cfg, ensure_ascii=False)
            if cfg_row is None:
                db.add(
                    UnifiedDeviceConfig(
                        device_id=device_id,
                        desired_json=rep_str,
                        reported_json=rep_str,
                        desired_ver=reported_ver,
                        reported_ver=reported_ver,
                        sync_status="synced",
                        updated_at=now,
                    )
                )
            else:
                sync_st = "pending" if int(cfg_row.desired_ver) > reported_ver else "synced"
                cfg_row.reported_json = rep_str
                cfg_row.reported_ver = reported_ver
                cfg_row.sync_status = sync_st
                cfg_row.updated_at = now

            _ensure_main_device(db, device_id, capabilities=_caps_dict_to_list(caps))
            db.add(
                UnifiedAuditLog(
                    device_id=device_id,
                    event="register",
                    detail=f"ip={ip} chip={chip} fw={fw_version}",
                    created_at=now,
                )
            )
            db.commit()
        return self.get_device(device_id)

    def process_heartbeat(self, device_id, payload):
        now = int(time.time())
        rssi = int(payload.get("rssi", 0) or 0)
        free_heap = int(payload.get("free_heap", 0) or 0)
        uptime_sec = int(payload.get("uptime_sec", 0) or 0)
        ip = str(payload.get("ip", ""))
        reported_ver = int(payload.get("config_ver", 1) or 1)

        sync_config = None
        cmds_out = []

        with _session_ctx() as db:
            dev = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == device_id).first()
            if not dev:
                return {"ok": False, "reregister": True}

            dev.status = "online"
            dev.rssi = rssi
            dev.free_heap = free_heap
            dev.uptime_sec = uptime_sec
            if ip:
                dev.ip = ip
            dev.last_seen_at = now

            cfg_row = db.query(UnifiedDeviceConfig).filter(UnifiedDeviceConfig.device_id == device_id).first()
            if cfg_row:
                desired_ver = int(cfg_row.desired_ver)
                if desired_ver > reported_ver:
                    sync_config = json.loads(cfg_row.desired_json or "{}")
                    sync_config["config_ver"] = desired_ver
                    cfg_row.sync_status = "pushing"
                    cfg_row.reported_ver = reported_ver
                elif cfg_row.sync_status != "synced":
                    cfg_row.sync_status = "synced"
                    cfg_row.reported_ver = reported_ver

            q_rows = (
                db.query(UnifiedPendingCommand)
                .filter(
                    UnifiedPendingCommand.device_id == device_id,
                    UnifiedPendingCommand.status == "queued",
                )
                .order_by(UnifiedPendingCommand.id.asc())
                .limit(5)
                .all()
            )
            for r in q_rows:
                cmds_out.append(
                    {
                        "id": r.id,
                        "type": r.cmd_type,
                        "payload": json.loads(r.payload_json or "{}"),
                    }
                )
                r.status = "sent"
                r.updated_at = now

            db.commit()

        return {"ok": True, "sync_config": sync_config, "commands": cmds_out}

    def list_devices(self, offline_ttl_sec=45):
        now = int(time.time())
        with _session_ctx() as db:
            rows = (
                db.query(UnifiedDevice)
                .order_by(UnifiedDevice.last_seen_at.desc(), UnifiedDevice.id.desc())
                .all()
            )
            cfgs = {c.device_id: c for c in db.query(UnifiedDeviceConfig).all()}
            out = []
            for r in rows:
                d = _row_to_device_dict(r, cfgs.get(r.device_id), strip_secrets=True)
                if d["status"] == "online" and d["last_seen_at"] and (now - int(d["last_seen_at"]) > offline_ttl_sec):
                    d["status"] = "offline"
                out.append(d)
            return out

    def get_device(self, device_id):
        with _session_ctx() as db:
            r = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == device_id).first()
            if not r:
                return None
            cfg = db.query(UnifiedDeviceConfig).filter(UnifiedDeviceConfig.device_id == device_id).first()
            return _row_to_device_dict(r, cfg, strip_secrets=False)

    def update_device_meta(self, device_id, name=None, claimed=None, api_token=None, device_secret=None):
        with _session_ctx() as db:
            dev = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == device_id).first()
            if not dev:
                return None
            if name is not None:
                dev.name = name
            if claimed is not None:
                dev.claimed = 1 if claimed else 0
            if api_token is not None:
                dev.api_token = encrypt_field(api_token) if api_token else ""
            if device_secret is not None:
                dev.device_secret = encrypt_field(device_secret) if device_secret else ""
            db.commit()
        return self.get_device(device_id)

    def update_desired_config(self, device_id, patch_dict):
        now = int(time.time())
        with _session_ctx() as db:
            cfg = db.query(UnifiedDeviceConfig).filter(UnifiedDeviceConfig.device_id == device_id).first()
            if cfg:
                cur = json.loads(cfg.desired_json or "{}")
                cur.update(patch_dict)
                new_ver = max(int(cfg.desired_ver), int(cfg.reported_ver)) + 1
                cfg.desired_json = json.dumps(cur, ensure_ascii=False)
                cfg.desired_ver = new_ver
                cfg.sync_status = "pending"
                cfg.updated_at = now
            else:
                new_ver = 2
                db.add(
                    UnifiedDeviceConfig(
                        device_id=device_id,
                        desired_json=json.dumps(patch_dict, ensure_ascii=False),
                        reported_json="{}",
                        desired_ver=new_ver,
                        reported_ver=0,
                        sync_status="pending",
                        updated_at=now,
                    )
                )
            db.add(
                UnifiedAuditLog(
                    device_id=device_id,
                    event="config_desired_updated",
                    detail=f"ver={new_ver}",
                    created_at=now,
                )
            )
            db.commit()
        return self.get_device(device_id)

    def queue_command(self, device_id, cmd_type, payload):
        now = int(time.time())
        with _session_ctx() as db:
            cmd = UnifiedPendingCommand(
                device_id=device_id,
                cmd_type=cmd_type,
                payload_json=json.dumps(payload or {}, ensure_ascii=False),
                status="queued",
                created_at=now,
                updated_at=now,
            )
            db.add(cmd)
            db.flush()
            cmd_id = cmd.id
            db.add(
                UnifiedAuditLog(
                    device_id=device_id,
                    event=f"cmd_queued:{cmd_type}",
                    detail=json.dumps(payload or {}, ensure_ascii=False),
                    created_at=now,
                )
            )
            db.commit()
        return cmd_id

    def delete_device(self, device_id):
        with _session_ctx() as db:
            db.query(UnifiedDeviceConfig).filter(UnifiedDeviceConfig.device_id == device_id).delete()
            db.query(UnifiedPendingCommand).filter(UnifiedPendingCommand.device_id == device_id).delete()
            cnt = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == device_id).delete()
            db.commit()
            return cnt > 0

    def list_audit_logs(self, limit=50):
        with _session_ctx() as db:
            rows = (
                db.query(UnifiedAuditLog)
                .order_by(UnifiedAuditLog.id.desc())
                .limit(int(limit))
                .all()
            )
            return [
                {
                    "id": r.id,
                    "device_id": r.device_id,
                    "event": r.event,
                    "detail": r.detail,
                    "created_at": r.created_at,
                }
                for r in rows
            ]

    def save_compiler_task(self, task: dict[str, Any]) -> None:
        tid = str(task.get("task_id", "")).strip()
        if not tid:
            return
        with _session_ctx() as db:
            row = db.query(UnifiedCompilerTask).filter(UnifiedCompilerTask.task_id == tid).first()
            if row is None:
                row = UnifiedCompilerTask(
                    task_id=tid,
                    kind=str(task.get("kind", "build"))[:16],
                    label=str(task.get("label", ""))[:255],
                    status=str(task.get("status", "queued"))[:32],
                    log=str(task.get("log", "")),
                    artifact_name=str(task.get("artifact_name", ""))[:255],
                    artifact_size=int(task.get("artifact_size", 0) or 0),
                    percent=int(task.get("percent", 0) or 0),
                    created_at=int(task.get("created_at", 0) or 0),
                    updated_at=int(task.get("updated_at", 0) or 0),
                )
                db.add(row)
            else:
                row.kind = str(task.get("kind", row.kind))[:16]
                row.label = str(task.get("label", row.label))[:255]
                row.status = str(task.get("status", row.status))[:32]
                row.log = str(task.get("log", row.log))
                row.artifact_name = str(task.get("artifact_name", row.artifact_name))[:255]
                row.artifact_size = int(task.get("artifact_size", row.artifact_size) or 0)
                row.percent = int(task.get("percent", row.percent) or 0)
                row.updated_at = int(task.get("updated_at", int(time.time())) or 0)
            db.commit()

    def load_compiler_tasks(self, limit: int = 30) -> list[dict[str, Any]]:
        with _session_ctx() as db:
            rows = (
                db.query(UnifiedCompilerTask)
                .order_by(UnifiedCompilerTask.created_at.desc())
                .limit(int(limit))
                .all()
            )
            return [
                {
                    "task_id": r.task_id,
                    "kind": r.kind,
                    "label": r.label,
                    "status": r.status,
                    "log": r.log,
                    "artifact_name": r.artifact_name,
                    "artifact_size": r.artifact_size,
                    "percent": r.percent,
                    "created_at": r.created_at,
                    "updated_at": r.updated_at,
                }
                for r in reversed(rows)
            ]

    def mark_stale_compiler_tasks_failed(self) -> int:
        now = int(time.time())
        with _session_ctx() as db:
            stale = (
                db.query(UnifiedCompilerTask)
                .filter(UnifiedCompilerTask.status.in_(("queued", "building", "flashing")))
                .all()
            )
            for row in stale:
                row.status = "failed"
                row.log = (row.log or "") + "\n⚠️ [系统恢复] 服务端曾重启，任务已终止。\n"
                row.updated_at = now
            if stale:
                db.commit()
            return len(stale)


def reconcile_stale_online_devices(db: Session) -> dict[str, int]:
    """Reset orphaned online device states & open connections after an ungraceful server exit."""
    now = datetime.now(timezone.utc)
    stale_devs = (
        db.query(Device)
        .filter(Device.status.in_(("online", "busy")))
        .update({Device.status: "offline"}, synchronize_session=False)
    )
    stale_conns = (
        db.query(DeviceConnection)
        .filter(DeviceConnection.disconnected_at.is_(None))
        .update({DeviceConnection.disconnected_at: now}, synchronize_session=False)
    )
    db.commit()
    return {"devices_reset": int(stale_devs or 0), "connections_closed": int(stale_conns or 0)}


def migrate_legacy_fleet_sqlite(sqlite_path: str) -> int:
    """Import pre-existing rows from legacy esp_unified_framework/data/fleet.db once."""
    if not sqlite_path or not os.path.isfile(sqlite_path):
        return 0
    src = sqlite3.connect(sqlite_path)
    src.row_factory = sqlite3.Row
    migrated = 0
    try:
        devs = src.execute("SELECT * FROM devices").fetchall()
        cfgs = {r["device_id"]: dict(r) for r in src.execute("SELECT * FROM device_configs").fetchall()}
        with _session_ctx() as db:
            for r in devs:
                did = r["device_id"]
                if db.query(UnifiedDevice).filter(UnifiedDevice.device_id == did).first():
                    continue
                db.add(
                    UnifiedDevice(
                        device_id=did,
                        mac=r["mac"] or "",
                        name=r["name"] or did,
                        chip=r["chip"] or "",
                        board=r["board"] or "",
                        flash_mb=int(r["flash_mb"] or 4),
                        psram_mb=int(r["psram_mb"] or 0),
                        ip=r["ip"] or "",
                        mdns_host=r["mdns_host"] or "",
                        web_port=int(r["web_port"] or 80),
                        fw_version=r["fw_version"] or "",
                        capabilities_json=r["capabilities_json"] or "{}",
                        status=r["status"] or "provisioned",
                        claimed=int(r["claimed"] or 0),
                        device_secret=encrypt_field(r["device_secret"] or ""),
                        api_token=encrypt_field(r["api_token"] or ""),
                        rssi=int(r["rssi"] or 0),
                        free_heap=int(r["free_heap"] or 0),
                        uptime_sec=int(r["uptime_sec"] or 0),
                        last_seen_at=int(r["last_seen_at"] or 0),
                        created_at=int(r["created_at"] or 0),
                    )
                )
                if did in cfgs and not db.query(UnifiedDeviceConfig).filter(UnifiedDeviceConfig.device_id == did).first():
                    c = cfgs[did]
                    db.add(
                        UnifiedDeviceConfig(
                            device_id=did,
                            desired_json=c["desired_json"] or "{}",
                            reported_json=c["reported_json"] or "{}",
                            desired_ver=int(c["desired_ver"] or 1),
                            reported_ver=int(c["reported_ver"] or 1),
                            sync_status=c["sync_status"] or "synced",
                            updated_at=int(c["updated_at"] or 0),
                        )
                    )
                _ensure_main_device(db, did, name=r["name"] or did)
                migrated += 1
            db.commit()
    finally:
        src.close()
    return migrated


sql_hub_store = SqlAlchemyHubStore()
