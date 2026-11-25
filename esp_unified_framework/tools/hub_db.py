#!/usr/bin/env python3
"""
ESP Unified Framework - SQLite Fleet Registry, Config Shadow & Audit Store
Provides durable device registration, pre-provisioning, HMAC secret storage,
config shadow synchronization, and queued remote commands.
"""

import os
import json
import time
import hmac
import hashlib
import secrets
import sqlite3
import threading

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DATA_DIR = os.path.join(PROJECT_ROOT, "data")
DB_PATH = os.path.join(DATA_DIR, "fleet.db")

_db_lock = threading.Lock()


def _get_conn():
    os.makedirs(DATA_DIR, exist_ok=True)
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")
    return conn


_CONN = None
_EXTERNAL_BACKEND = None


def set_backend(backend):
    """Register an external store backend (e.g. py_web SQLAlchemy Store)."""
    global _EXTERNAL_BACKEND
    _EXTERNAL_BACKEND = backend


def init_db(db_path=None):
    global DB_PATH, _CONN, _EXTERNAL_BACKEND
    if db_path is None and _EXTERNAL_BACKEND is not None:
        return
    if db_path is not None:
        # Explicit sqlite file path bypasses external backend for standalone CLI/tests
        _EXTERNAL_BACKEND = None
        DB_PATH = db_path
    with _db_lock:
        if _CONN is not None:
            _CONN.close()
        _CONN = _get_conn()
        _CONN.executescript(
            """
            CREATE TABLE IF NOT EXISTS devices (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT UNIQUE NOT NULL,
                mac TEXT DEFAULT '',
                name TEXT DEFAULT '',
                chip TEXT DEFAULT '',
                board TEXT DEFAULT '',
                flash_mb INTEGER DEFAULT 4,
                psram_mb INTEGER DEFAULT 0,
                ip TEXT DEFAULT '',
                mdns_host TEXT DEFAULT '',
                web_port INTEGER DEFAULT 80,
                fw_version TEXT DEFAULT '',
                capabilities_json TEXT DEFAULT '{}',
                status TEXT DEFAULT 'provisioned',
                claimed INTEGER DEFAULT 0,
                device_secret TEXT DEFAULT '',
                api_token TEXT DEFAULT '',
                rssi INTEGER DEFAULT 0,
                free_heap INTEGER DEFAULT 0,
                uptime_sec INTEGER DEFAULT 0,
                last_seen_at INTEGER DEFAULT 0,
                created_at INTEGER DEFAULT 0
            );

            CREATE TABLE IF NOT EXISTS device_configs (
                device_id TEXT PRIMARY KEY,
                desired_json TEXT DEFAULT '{}',
                reported_json TEXT DEFAULT '{}',
                desired_ver INTEGER DEFAULT 1,
                reported_ver INTEGER DEFAULT 1,
                sync_status TEXT DEFAULT 'synced',
                updated_at INTEGER DEFAULT 0
            );

            CREATE TABLE IF NOT EXISTS pending_commands (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                cmd_type TEXT NOT NULL,
                payload_json TEXT NOT NULL,
                status TEXT DEFAULT 'queued',
                created_at INTEGER DEFAULT 0,
                updated_at INTEGER DEFAULT 0
            );

            CREATE TABLE IF NOT EXISTS auth_nonces (
                nonce TEXT PRIMARY KEY,
                device_id TEXT,
                expires_at INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS audit_logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT DEFAULT '',
                event TEXT NOT NULL,
                detail TEXT DEFAULT '',
                created_at INTEGER DEFAULT 0
            );
            """
        )
        _CONN.commit()


def ensure_init():
    if _EXTERNAL_BACKEND is not None:
        return
    if _CONN is None:
        init_db()


def close_db():
    global _CONN
    if _EXTERNAL_BACKEND is not None:
        _EXTERNAL_BACKEND.close_db()
        return
    with _db_lock:
        if _CONN is not None:
            _CONN.close()
            _CONN = None


def log_audit(device_id, event, detail=""):
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.log_audit(device_id, event, detail)
    ensure_init()
    now = int(time.time())
    with _db_lock:
        _CONN.execute(
            "INSERT INTO audit_logs (device_id, event, detail, created_at) VALUES (?, ?, ?, ?)",
            (device_id or "", event, detail, now),
        )
        _CONN.commit()


def issue_nonce(device_id=""):
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.issue_nonce(device_id)
    ensure_init()
    nonce = secrets.token_hex(16)
    now = int(time.time())
    with _db_lock:
        _CONN.execute("DELETE FROM auth_nonces WHERE expires_at < ?", (now,))
        _CONN.execute(
            "INSERT INTO auth_nonces (nonce, device_id, expires_at) VALUES (?, ?, ?)",
            (nonce, device_id, now + 120),
        )
        _CONN.commit()
    return nonce


def verify_and_consume_hmac(device_id, nonce, proof_hex, global_secret=""):
    """Verify HMAC-SHA256(secret, nonce) using device-specific secret or global fallback."""
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.verify_and_consume_hmac(device_id, nonce, proof_hex, global_secret)
    ensure_init()
    now = int(time.time())
    with _db_lock:
        row = _CONN.execute(
            "SELECT * FROM auth_nonces WHERE nonce = ?", (nonce,)
        ).fetchone()
        if not row or int(row["expires_at"]) < now:
            return False, "invalid or expired nonce"
        _CONN.execute("DELETE FROM auth_nonces WHERE nonce = ?", (nonce,))

        dev = _CONN.execute(
            "SELECT device_secret FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()
        _CONN.commit()

    secret = ""
    if dev and dev["device_secret"]:
        secret = dev["device_secret"]
    elif global_secret:
        secret = global_secret

    if not secret:
        return True, "no_secret_configured"

    expected = hmac.new(
        secret.encode("utf-8"), nonce.encode("utf-8"), hashlib.sha256
    ).hexdigest()
    if hmac.compare_digest(expected, str(proof_hex or "").lower()):
        return True, "verified"
    return False, "hmac mismatch"


def provision_device(device_id, mac="", chip="", flash_mb=4, board="", device_secret="", hub_url="", allow_empty_secret=False):
    """Pre-register a device during USB flashing / factory testing."""
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.provision_device(
            device_id, mac=mac, chip=chip, flash_mb=flash_mb, board=board,
            device_secret=device_secret, hub_url=hub_url, allow_empty_secret=allow_empty_secret,
        )
    ensure_init()
    now = int(time.time())
    if not device_secret and not allow_empty_secret:
        device_secret = secrets.token_hex(16)

    with _db_lock:
        existing = _CONN.execute(
            "SELECT * FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()
        if existing:
            sec_sql = "?" if allow_empty_secret else "COALESCE(NULLIF(?,''), device_secret)"
            _CONN.execute(
                f"""
                UPDATE devices SET mac=COALESCE(NULLIF(?,''), mac),
                    chip=COALESCE(NULLIF(?,''), chip),
                    flash_mb=CASE WHEN ?>0 THEN ? ELSE flash_mb END,
                    board=COALESCE(NULLIF(?,''), board),
                    device_secret={sec_sql},
                    status=CASE WHEN status='online' THEN 'online' ELSE 'provisioned' END
                WHERE device_id=?
                """,
                (mac, chip, flash_mb, flash_mb, board, device_secret, device_id),
            )
        else:
            _CONN.execute(
                """
                INSERT INTO devices (
                    device_id, mac, name, chip, board, flash_mb,
                    status, claimed, device_secret, created_at
                ) VALUES (?, ?, ?, ?, ?, ?, 'provisioned', 0, ?, ?)
                """,
                (device_id, mac, device_id, chip, board, flash_mb, device_secret, now),
            )

        initial_cfg = {"device_id": device_id}
        if hub_url:
            initial_cfg["hub_url"] = hub_url
        init_str = json.dumps(initial_cfg)
        cfg_exists = _CONN.execute(
            "SELECT 1 FROM device_configs WHERE device_id = ?", (device_id,)
        ).fetchone()
        if not cfg_exists:
            _CONN.execute(
                "INSERT INTO device_configs (device_id, desired_json, reported_json, desired_ver, reported_ver, sync_status, updated_at) VALUES (?, ?, ?, 1, 1, 'synced', ?)",
                (device_id, init_str, init_str, now),
            )
        else:
            _CONN.execute(
                "UPDATE device_configs SET desired_json=?, reported_json=?, desired_ver=1, reported_ver=1, sync_status='synced', updated_at=? WHERE device_id=?",
                (init_str, init_str, now, device_id),
            )
        _CONN.commit()

    log_audit(device_id, "provisioned", f"chip={chip} mac={mac} flash={flash_mb}MB")
    return get_device(device_id)


def register_or_update_device(payload):
    """Called when a device boots onto WiFi and checks in with the Fleet Hub."""
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.register_or_update_device(payload)
    ensure_init()
    now = int(time.time())
    device_id = payload.get("device_id", "").strip()
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

    with _db_lock:
        existing = _CONN.execute(
            "SELECT * FROM devices WHERE device_id = ?", (device_id,)
        ).fetchone()
        if existing:
            new_status = "online"
            _CONN.execute(
                """
                UPDATE devices SET
                    mac=COALESCE(NULLIF(?,''), mac),
                    chip=COALESCE(NULLIF(?,''), chip),
                    board=COALESCE(NULLIF(?,''), board),
                    flash_mb=CASE WHEN ?>0 THEN ? ELSE flash_mb END,
                    psram_mb=?,
                    ip=COALESCE(NULLIF(?,''), ip),
                    mdns_host=COALESCE(NULLIF(?,''), mdns_host),
                    web_port=?,
                    fw_version=COALESCE(NULLIF(?,''), fw_version),
                    capabilities_json=?,
                    status=?,
                    last_seen_at=?
                WHERE device_id=?
                """,
                (
                    mac, chip, board, flash_mb, flash_mb, psram_mb,
                    ip, mdns_host, web_port, fw_version, caps_json,
                    new_status, now, device_id,
                ),
            )
        else:
            _CONN.execute(
                """
                INSERT INTO devices (
                    device_id, mac, name, chip, board, flash_mb, psram_mb,
                    ip, mdns_host, web_port, fw_version, capabilities_json,
                    status, claimed, last_seen_at, created_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'online', 0, ?, ?)
                """,
                (
                    device_id, mac, device_id, chip, board, flash_mb, psram_mb,
                    ip, mdns_host, web_port, fw_version, caps_json, now, now,
                ),
            )

        # Update reported config in shadow store
        cfg_row = _CONN.execute(
            "SELECT * FROM device_configs WHERE device_id = ?", (device_id,)
        ).fetchone()
        rep_str = json.dumps(reported_cfg, ensure_ascii=False)
        if not cfg_row:
            _CONN.execute(
                "INSERT INTO device_configs (device_id, desired_json, reported_json, desired_ver, reported_ver, sync_status, updated_at) VALUES (?, ?, ?, ?, ?, 'synced', ?)",
                (device_id, rep_str, rep_str, reported_ver, reported_ver, now),
            )
        else:
            desired_ver = int(cfg_row["desired_ver"])
            sync_st = "pending" if desired_ver > reported_ver else "synced"
            _CONN.execute(
                "UPDATE device_configs SET reported_json=?, reported_ver=?, sync_status=?, updated_at=? WHERE device_id=?",
                (rep_str, reported_ver, sync_st, now, device_id),
            )
        _CONN.commit()

    log_audit(device_id, "register", f"ip={ip} chip={chip} fw={fw_version}")
    return get_device(device_id)


def process_heartbeat(device_id, payload):
    """Update live telemetry and return any pending shadow config or queued commands."""
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.process_heartbeat(device_id, payload)
    ensure_init()
    now = int(time.time())
    rssi = int(payload.get("rssi", 0) or 0)
    free_heap = int(payload.get("free_heap", 0) or 0)
    uptime_sec = int(payload.get("uptime_sec", 0) or 0)
    ip = str(payload.get("ip", ""))
    reported_ver = int(payload.get("config_ver", 1) or 1)

    sync_config = None
    cmds_out = []

    with _db_lock:
        dev = _CONN.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone()
        if not dev:
            return {"ok": False, "reregister": True}

        _CONN.execute(
            """
            UPDATE devices SET
                status='online', rssi=?, free_heap=?, uptime_sec=?,
                ip=COALESCE(NULLIF(?,''), ip), last_seen_at=?
            WHERE device_id=?
            """,
            (rssi, free_heap, uptime_sec, ip, now, device_id),
        )

        # Check config shadow diff
        cfg_row = _CONN.execute(
            "SELECT * FROM device_configs WHERE device_id=?", (device_id,)
        ).fetchone()
        if cfg_row:
            desired_ver = int(cfg_row["desired_ver"])
            if desired_ver > reported_ver:
                sync_config = json.loads(cfg_row["desired_json"] or "{}")
                sync_config["config_ver"] = desired_ver
                _CONN.execute(
                    "UPDATE device_configs SET sync_status='pushing', reported_ver=? WHERE device_id=?",
                    (reported_ver, device_id),
                )
            elif cfg_row["sync_status"] != "synced":
                _CONN.execute(
                    "UPDATE device_configs SET sync_status='synced', reported_ver=? WHERE device_id=?",
                    (reported_ver, device_id),
                )

        # Pop queued commands
        q_rows = _CONN.execute(
            "SELECT id, cmd_type, payload_json FROM pending_commands WHERE device_id=? AND status='queued' ORDER BY id ASC LIMIT 5",
            (device_id,),
        ).fetchall()
        for r in q_rows:
            cmds_out.append({
                "id": r["id"],
                "type": r["cmd_type"],
                "payload": json.loads(r["payload_json"]),
            })
            _CONN.execute(
                "UPDATE pending_commands SET status='sent', updated_at=? WHERE id=?",
                (now, r["id"]),
            )
        _CONN.commit()

    return {
        "ok": True,
        "sync_config": sync_config,
        "commands": cmds_out,
    }


def list_devices(offline_ttl_sec=45):
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.list_devices(offline_ttl_sec)
    ensure_init()
    now = int(time.time())
    with _db_lock:
        rows = _CONN.execute(
            "SELECT * FROM devices ORDER BY last_seen_at DESC, id DESC"
        ).fetchall()
        cfg_rows = _CONN.execute("SELECT * FROM device_configs").fetchall()

    cfg_map = {r["device_id"]: dict(r) for r in cfg_rows}
    out = []
    for r in rows:
        d = dict(r)
        # Compute effective online status cleanly via TTL
        if d["status"] == "online" and d["last_seen_at"] and (now - int(d["last_seen_at"]) > offline_ttl_sec):
            d["status"] = "offline"
        try:
            d["capabilities"] = json.loads(d.get("capabilities_json") or "{}")
        except Exception:
            d["capabilities"] = {}
        d.pop("capabilities_json", None)
        d["has_secret"] = bool(d.pop("device_secret", ""))
        d["has_api_token"] = bool(d.pop("api_token", ""))
        d["shadow"] = cfg_map.get(d["device_id"], {})
        out.append(d)
    return out


def get_device(device_id):
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.get_device(device_id)
    ensure_init()
    with _db_lock:
        r = _CONN.execute("SELECT * FROM devices WHERE device_id=?", (device_id,)).fetchone()
        if not r:
            return None
        cfg = _CONN.execute("SELECT * FROM device_configs WHERE device_id=?", (device_id,)).fetchone()
    d = dict(r)
    try:
        d["capabilities"] = json.loads(d.get("capabilities_json") or "{}")
    except Exception:
        d["capabilities"] = {}
    d["has_secret"] = bool(d.get("device_secret", ""))
    d["shadow"] = dict(cfg) if cfg else {}
    return d


def update_device_meta(device_id, name=None, claimed=None, api_token=None):
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.update_device_meta(device_id, name=name, claimed=claimed, api_token=api_token)
    ensure_init()
    with _db_lock:
        dev = _CONN.execute("SELECT * FROM devices WHERE device_id=?", (device_id,)).fetchone()
        if not dev:
            return None
        if name is not None:
            _CONN.execute("UPDATE devices SET name=? WHERE device_id=?", (name, device_id))
        if claimed is not None:
            _CONN.execute("UPDATE devices SET claimed=? WHERE device_id=?", (1 if claimed else 0, device_id))
        if api_token is not None:
            _CONN.execute("UPDATE devices SET api_token=? WHERE device_id=?", (api_token, device_id))
        _CONN.commit()
    return get_device(device_id)


def update_desired_config(device_id, patch_dict):
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.update_desired_config(device_id, patch_dict)
    ensure_init()
    now = int(time.time())
    with _db_lock:
        cfg_row = _CONN.execute("SELECT * FROM device_configs WHERE device_id=?", (device_id,)).fetchone()
        if cfg_row:
            cur = json.loads(cfg_row["desired_json"] or "{}")
            cur.update(patch_dict)
            new_ver = max(int(cfg_row["desired_ver"]), int(cfg_row["reported_ver"])) + 1
            _CONN.execute(
                "UPDATE device_configs SET desired_json=?, desired_ver=?, sync_status='pending', updated_at=? WHERE device_id=?",
                (json.dumps(cur, ensure_ascii=False), new_ver, now, device_id),
            )
        else:
            new_ver = 2
            _CONN.execute(
                "INSERT INTO device_configs (device_id, desired_json, reported_json, desired_ver, reported_ver, sync_status, updated_at) VALUES (?, ?, '{}', ?, 0, 'pending', ?)",
                (device_id, json.dumps(patch_dict, ensure_ascii=False), new_ver, now),
            )
        _CONN.commit()
    log_audit(device_id, "config_desired_updated", f"ver={new_ver}")
    return get_device(device_id)


def queue_command(device_id, cmd_type, payload):
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.queue_command(device_id, cmd_type, payload)
    ensure_init()
    now = int(time.time())
    with _db_lock:
        cur = _CONN.execute(
            "INSERT INTO pending_commands (device_id, cmd_type, payload_json, status, created_at, updated_at) VALUES (?, ?, ?, 'queued', ?, ?)",
            (device_id, cmd_type, json.dumps(payload or {}, ensure_ascii=False), now, now),
        )
        cmd_id = cur.lastrowid
        _CONN.commit()
    log_audit(device_id, f"cmd_queued:{cmd_type}", json.dumps(payload or {}, ensure_ascii=False))
    return cmd_id


def delete_device(device_id):
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.delete_device(device_id)
    ensure_init()
    with _db_lock:
        _CONN.execute("DELETE FROM device_configs WHERE device_id=?", (device_id,))
        _CONN.execute("DELETE FROM pending_commands WHERE device_id=?", (device_id,))
        cur = _CONN.execute("DELETE FROM devices WHERE device_id=?", (device_id,))
        _CONN.commit()
        return cur.rowcount > 0


def list_audit_logs(limit=50):
    if _EXTERNAL_BACKEND is not None:
        return _EXTERNAL_BACKEND.list_audit_logs(limit)
    ensure_init()
    with _db_lock:
        rows = _CONN.execute(
            "SELECT * FROM audit_logs ORDER BY id DESC LIMIT ?", (int(limit),)
        ).fetchall()
    return [dict(r) for r in rows]
