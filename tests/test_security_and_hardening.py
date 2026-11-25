"""Comprehensive regression tests for security hardening, persistence, and sync fixes."""

import asyncio
import hashlib
import hmac
import io
import time
import uuid

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app.core.config.config import settings, validate_runtime_security
from app.core.security import create_download_token, verify_download_token
from app.core.wifi_crypto import decrypt_field, encrypt_field
from app.database.crud import device as crud_device
from app.database.models.device import Device, DeviceConnection
from app.database.models.unified_fleet import UnifiedCompilerTask, UnifiedDevice
from app.main import app
from app.services.device_registry import device_registry
from app.services.idempotency import seen_or_record_async
from app.services.ota_tracker import OtaTracker
from app.services.queue import RedisBackend
from app.services.unified_store import reconcile_stale_online_devices, sql_hub_store
from tests.conftest import TestingSessionLocal, test_engine


def _reg_login(client, username: str, role: str = "user") -> dict[str, str]:
    client.post(
        "/api/v1/auth/register",
        json={"username": username, "password": "Password123", "role": role},
    )
    tok = client.post(
        "/api/v1/auth/login",
        json={"username": username, "password": "Password123"},
    ).json()["access_token"]
    return {"Authorization": f"Bearer {tok}"}


def test_firmware_download_auth_signature_upload_sanitize_and_chip_check(client, db, tmp_path, monkeypatch):
    import app.api.v2.firmwares as fw_mod

    monkeypatch.setattr(fw_mod, "UPLOAD_DIR", tmp_path)
    adm_hdrs = _reg_login(client, "sec_adm", role="admin")

    # Path traversal in filename should be sanitized to basename only
    bin_bytes = b"\xE9\x01\x02" * 200
    up = client.post(
        "/api/v2/firmwares/upload",
        headers=adm_hdrs,
        data={"version": "2.0.0", "target_chip": "esp32s3"},
        files={"file": ("../../evil_sub/fw_s3.bin", io.BytesIO(bin_bytes), "application/octet-stream")},
    )
    assert up.status_code == 200
    fw_data = up.json()
    assert fw_data["filename"] == "fw_s3.bin"
    saved_files = list(tmp_path.iterdir())
    assert len(saved_files) == 1
    assert saved_files[0].is_file() and saved_files[0].name.endswith("_fw_s3.bin")

    fw_id = fw_data["id"]
    # Anonymous download rejected, tampered sig rejected, expired sig rejected
    assert client.get(f"/api/v2/firmwares/{fw_id}/download").status_code == 401
    assert client.get(f"/api/v2/firmwares/{fw_id}/download", params={"sig": "deadbeef.01234567"}).status_code == 401
    expired = create_download_token(f"fw:{fw_id}", ttl_sec=1)
    assert verify_download_token(f"fw:{fw_id}", expired) is True
    assert client.get(f"/api/v2/firmwares/{fw_id}/download", params={"sig": f"{int(time.time()) - 10:x}.{'0'*24}"}).status_code == 401

    # Valid signed URL works
    sig = fw_data["download_url"].split("sig=", 1)[1]
    ok_dl = client.get(f"/api/v2/firmwares/{fw_id}/download", params={"sig": sig})
    assert ok_dl.status_code == 200
    assert ok_dl.content == bin_bytes

    # Compatibility toggle restores anonymous download when needed
    monkeypatch.setattr(settings, "ALLOW_ANONYMOUS_FIRMWARE_DOWNLOAD", True)
    assert client.get(f"/api/v2/firmwares/{fw_id}/download").status_code == 200

    # Push checks UnifiedDevice chip compatibility when registered
    db.add(Device(device_uuid="c3_mismatch_dev", name="C3"))
    db.add(UnifiedDevice(device_id="c3_mismatch_dev", chip="ESP32-C3"))
    db.commit()
    push_res = client.post(
        f"/api/v2/firmwares/{fw_id}/push",
        headers=adm_hdrs,
        json={"device_uuids": ["c3_mismatch_dev"]},
    )
    assert push_res.status_code == 200
    r0 = push_res.json()["results"][0]
    assert r0["status"] == "failed"
    assert "Target mismatch" in r0["error"]


def test_firmware_progress_status_and_host_meta_idor_blocked(client, db):
    u1_hdrs = _reg_login(client, "idor_u1")
    u2_hdrs = _reg_login(client, "idor_u2")

    res = client.post(
        "/api/v1/devices",
        headers=u1_hdrs,
        json={"device_uuid": "idor_dev_01", "name": "Mine"},
    )
    assert res.status_code == 201

    assert client.get("/api/v2/firmwares/progress/idor_dev_01", headers=u2_hdrs).status_code == 403
    assert client.get("/api/v2/firmwares/devices/idor_dev_01/status", headers=u2_hdrs).status_code == 403
    assert client.get("/api/v2/devices/idor_dev_01/host-meta", headers=u2_hdrs).status_code == 403
    assert client.get("/api/v2/firmwares/progress/idor_dev_01", headers=u1_hdrs).status_code == 200


def test_scripts_admin_only_path_traversal_blocked_and_async_runner(client, tmp_path, monkeypatch):
    import app.api.v1.scripts as scripts_mod

    adm_hdrs = _reg_login(client, "script_adm", role="admin")
    usr_hdrs = _reg_login(client, "script_usr", role="user")

    sdir = tmp_path / "scripts"
    sdir.mkdir()
    (sdir / "hello.py").write_text("print('script-ok-line')\n", encoding="utf-8")
    (sdir / "notes.txt").write_text("not executable", encoding="utf-8")
    monkeypatch.setattr(scripts_mod, "SCRIPTS_DIR", sdir)

    # Regular user forbidden from running scripts
    r_forbid = client.post(
        "/api/v1/scripts/run",
        params={"script": "hello.py", "config_name": "default"},
        headers=usr_hdrs,
    )
    assert r_forbid.status_code == 403

    # Traversal and unsupported extension blocked even for admin
    assert client.post(
        "/api/v1/scripts/run",
        params={"script": "../secret.py", "config_name": "default"},
        headers=adm_hdrs,
    ).status_code == 400
    assert client.get(
        "/api/v1/scripts/configs",
        params={"script": "../etc"},
        headers=adm_hdrs,
    ).status_code == 400
    assert client.post(
        "/api/v1/scripts/run",
        params={"script": "notes.txt", "config_name": "default"},
        headers=adm_hdrs,
    ).status_code == 400

    # Valid script runs cleanly and publishes output to log_bus
    async def _run_and_read():
        from app.services.log_bus import log_bus
        from app.services.script_config import ScriptConfig
        from app.services.script_runner import run_script_async

        exec_id = "test-exec-1"
        received = []

        async def _consume():
            async for line in log_bus.subscribe(exec_id):
                received.append(line)

        consumer = asyncio.create_task(_consume())
        await asyncio.sleep(0.02)
        run_script_async(
            ScriptConfig(script_path=sdir / "hello.py", workdir=sdir, interpreter=None, args=[], env={}),
            exec_id,
            loop=asyncio.get_running_loop(),
        )
        await asyncio.wait_for(consumer, timeout=5)
        return received

    lines = asyncio.run(_run_and_read())
    assert "script-ok-line" in lines
    assert any(l.startswith("[EXIT] code=0") for l in lines)


def test_registration_lockdown_and_weak_jwt_guard(client, monkeypatch):
    # Initial admin bootstrap succeeds even if ALLOW_SELF_REGISTER=False
    monkeypatch.setattr(settings, "ALLOW_SELF_REGISTER", False)
    st0 = client.get("/api/v1/auth/bootstrap-status").json()
    assert st0["has_admin"] is False
    assert st0["allow_register"] is True

    adm_hdrs = _reg_login(client, "lock_adm", role="admin")
    st1 = client.get("/api/v1/auth/bootstrap-status").json()
    assert st1["has_admin"] is True
    assert st1["allow_register"] is False

    # Public registration blocked once admin exists, but admin can still create users
    assert client.post(
        "/api/v1/auth/register",
        json={"username": "stranger", "password": "Password123", "role": "user"},
    ).status_code == 403
    assert client.post(
        "/api/v1/auth/register",
        headers=adm_hdrs,
        json={"username": "invited_u", "password": "Password123", "role": "user"},
    ).status_code == 201

    # Non-debug weak JWT check raises RuntimeError when forced
    monkeypatch.setattr(settings, "DEBUG", False)
    monkeypatch.setattr(settings, "JWT_SECRET", "change-me-please")
    with pytest.raises(RuntimeError):
        validate_runtime_security(force=True)

    monkeypatch.setattr(settings, "JWT_SECRET", "a-very-strong-random-test-secret-key")
    validate_runtime_security(force=True)


def test_fleet_compat_strict_auth_and_device_id_validation(client, monkeypatch):
    from app.services import unified_bridge as ub

    ub.init_unified_hub()
    sec = "fleet_dev_secret_123456"
    ub.hub_db.provision_device(device_id="esp32s3-hmac01", device_secret=sec)

    # Bad device_id rejected
    assert client.get("/api/fleet/challenge", params={"device_id": "../bad"}).status_code == 400
    assert client.post("/api/fleet/heartbeat", json={"device_id": "bad/id"}).status_code == 400

    # Strict mode rejects unsigned heartbeat & cmd_ack
    monkeypatch.setattr(settings, "FLEET_STRICT_DEVICE_AUTH", True)
    assert client.post("/api/fleet/heartbeat", json={"device_id": "esp32s3-hmac01"}).status_code == 401
    assert client.post("/api/fleet/cmd_ack", json={"device_id": "esp32s3-hmac01", "cmd_id": 1}).status_code == 401

    # Valid challenge HMAC header succeeds
    nonce = client.get("/api/fleet/challenge", params={"device_id": "esp32s3-hmac01"}).json()["nonce"]
    proof = hmac.new(sec.encode(), nonce.encode(), hashlib.sha256).hexdigest()
    hb_ok = client.post(
        "/api/fleet/heartbeat",
        headers={"X-Device-Nonce": nonce, "X-Device-Proof": proof},
        json={"device_id": "esp32s3-hmac01", "rssi": -50},
    )
    assert hb_ok.status_code == 200
    assert hb_ok.json()["ok"] is True

    # Replay of same nonce fails even without strict mode
    monkeypatch.setattr(settings, "FLEET_STRICT_DEVICE_AUTH", False)
    hb_replay = client.post(
        "/api/fleet/heartbeat",
        headers={"X-Device-Nonce": nonce, "X-Device-Proof": proof},
        json={"device_id": "esp32s3-hmac01"},
    )
    assert hb_replay.status_code == 401


def test_ws_device_blocks_mid_session_id_switch_and_invalid_id(monkeypatch):
    monkeypatch.setattr("app.api.ws.device.SessionLocal", TestingSessionLocal)
    monkeypatch.setattr("app.database.session.engine", test_engine)
    try:
        with TestClient(app) as tc:
            # Invalid device_id rejected
            with tc.websocket_connect("/ws/device") as ws:
                ws.send_json({
                    "header": {"msg_id": str(uuid.uuid4()), "type": "register", "timestamp": 0},
                    "payload": {"device_id": "../bad/id"},
                })
                err = ws.receive_json()
                assert err["payload"]["code"] == "E004"
                with pytest.raises(WebSocketDisconnect):
                    ws.receive_json()

            # Registered socket cannot switch to another device_id
            with tc.websocket_connect("/ws/device") as ws:
                ws.send_json({
                    "header": {"msg_id": str(uuid.uuid4()), "type": "register", "timestamp": 0},
                    "payload": {"device_id": "ws_dev_one", "capabilities": []},
                })
                assert ws.receive_json()["payload"]["status"] == "ok"

                ws.send_json({
                    "header": {"msg_id": str(uuid.uuid4()), "type": "register", "timestamp": 0},
                    "payload": {"device_id": "ws_dev_two", "capabilities": []},
                })
                err2 = ws.receive_json()
                assert err2["payload"]["code"] == "E403"
                with pytest.raises(WebSocketDisconnect):
                    ws.receive_json()
    finally:
        device_registry._live.clear()


def test_unified_secret_encryption_and_crypto_key_separation(db, monkeypatch):
    monkeypatch.setattr(settings, "WIFI_CRYPTO_KEY", "dedicated-wifi-master-key-xyz")
    sql_hub_store.provision_device(
        device_id="enc-dev-01",
        device_secret="plain-device-secret",
    )
    sql_hub_store.update_device_meta("enc-dev-01", api_token="plain-api-token")

    # Raw DB columns must store encrypted enc:v1: payloads, never plaintext
    raw_row = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == "enc-dev-01").first()
    assert raw_row.device_secret.startswith("enc:v1:")
    assert "plain-device-secret" not in raw_row.device_secret
    assert raw_row.api_token.startswith("enc:v1:")
    assert "plain-api-token" not in raw_row.api_token

    # Changing JWT_SECRET does NOT break decryption when WIFI_CRYPTO_KEY is set
    monkeypatch.setattr(settings, "JWT_SECRET", "rotated-jwt-secret-key")
    dev = sql_hub_store.get_device("enc-dev-01")
    assert dev["device_secret"] == "plain-device-secret"
    assert dev["api_token"] == "plain-api-token"

    # HMAC challenge verification decrypts stored device_secret transparently
    nonce = sql_hub_store.issue_nonce("enc-dev-01")
    proof = hmac.new(b"plain-device-secret", nonce.encode(), hashlib.sha256).hexdigest()
    ok, _ = sql_hub_store.verify_and_consume_hmac("enc-dev-01", nonce, proof)
    assert ok is True

    # Legacy plaintext row remains backward-compatible
    assert decrypt_field("legacy-unencrypted") == "legacy-unencrypted"
    assert encrypt_field("enc:v1:already") == "enc:v1:already"


def test_compiler_task_db_persistence_and_crash_recovery(db):
    from app.services import unified_bridge as ub

    wc = ub.web_compiler
    wc.set_task_store(sql_hub_store.save_compiler_task, sql_hub_store.load_compiler_tasks)
    tid = wc.create_task("build", "persisted-build (esp32s3)", status="building", initial_log="boot\n")
    wc._update_task(tid, append_log="compiling...\n", percent=45)

    row = db.query(UnifiedCompilerTask).filter(UnifiedCompilerTask.task_id == tid).first()
    assert row is not None
    assert row.status == "building"
    assert "compiling..." in row.log

    # Simulate restart recovery: in-flight task is marked failed cleanly and rehydrated
    failed_cnt = sql_hub_store.mark_stale_compiler_tasks_failed()
    assert failed_cnt >= 1

    with wc.s_lock:
        wc.s_tasks.clear()
        wc.s_task_order.clear()
        wc.s_latest_task_id = None

    wc.set_task_store(sql_hub_store.save_compiler_task, sql_hub_store.load_compiler_tasks)
    snap = wc.get_task_snapshot(tid)
    assert snap["status"] == "failed"
    assert "服务端曾重启" in snap["log"]


def test_reconcile_stale_online_devices_and_dual_table_sync(client, db):
    u_hdrs = _reg_login(client, "sync_u1")
    sql_hub_store.provision_device(device_id="sync-dev-01", device_secret="sec")

    # Lookup primary Device id & claim + rename through /api/v1/devices
    d = crud_device.get_by_uuid(db, "sync-dev-01")
    assert d is not None
    assert client.post(f"/api/v1/devices/{d.id}/claim", headers=u_hdrs).status_code == 200
    assert client.put(
        f"/api/v1/devices/{d.id}",
        headers=u_hdrs,
        json={"name": "Synced-Name"},
    ).status_code == 200

    db.expire_all()
    ud = db.query(UnifiedDevice).filter(UnifiedDevice.device_id == "sync-dev-01").first()
    assert ud.claimed == 1
    assert ud.name == "Synced-Name"

    # Simulate unclean exit leaving online device + unclosed connection
    d.status = "online"
    db.add(DeviceConnection(device_id=d.id, ip="10.0.0.2", session_id="stale-sess"))
    db.commit()

    stats = reconcile_stale_online_devices(db)
    assert stats["devices_reset"] == 1
    assert stats["connections_closed"] == 1
    db.expire_all()
    assert crud_device.get_by_id(db, d.id).status == "offline"


def test_redis_queue_idempotency_ota_and_health_endpoints(client, monkeypatch):
    class _FakeRedis:
        def __init__(self):
            self.lists = {}
            self.kv = {}
            self.hashes = {}
            self.ttls = {}

        async def lpush(self, key, val):
            self.lists.setdefault(key, []).insert(0, val)

        async def rpush(self, key, val):
            self.lists.setdefault(key, []).append(val)

        async def brpop(self, key, timeout=1):
            lst = self.lists.get(key) or []
            if not lst:
                return None
            return (key, lst.pop())

        async def llen(self, key):
            return len(self.lists.get(key) or [])

        async def expire(self, key, ttl):
            self.ttls[key] = ttl

        async def set(self, key, val, ex=None, nx=False):
            if nx and key in self.kv:
                return False
            self.kv[key] = val
            return True

        def hset(self, name, key, val):
            self.hashes.setdefault(name, {})[key] = val

        def hget(self, name, key):
            return self.hashes.get(name, {}).get(key)

        def hgetall(self, name):
            return dict(self.hashes.get(name, {}))

        def hdel(self, name, key):
            self.hashes.get(name, {}).pop(key, None)

    fake_r = _FakeRedis()
    monkeypatch.setattr("app.core.cache.get_redis", lambda: fake_r)
    monkeypatch.setattr(settings, "QUEUE_BACKEND", "redis")

    async def _exercise():
        qb = RedisBackend()
        await qb.push("d1", {"seq": 1})
        await qb.push("d1", {"seq": 2})
        assert await qb.size("d1") == 2
        first = await qb.pop("d1")
        assert first == {"seq": 1}
        # requeue puts failed frame back at consumer tail (FIFO head), ahead of seq=2
        await qb.requeue("d1", first)
        assert await qb.pop("d1") == {"seq": 1}
        assert await qb.pop("d1") == {"seq": 2}
        assert fake_r.ttls["queue:device:d1"] == 3600

        # Idempotency via Redis SET NX EX
        assert await seen_or_record_async("msg-uniq-1") is False
        assert await seen_or_record_async("msg-uniq-1") is True

    asyncio.run(_exercise())

    tracker = OtaTracker()
    tracker._sync_redis = fake_r
    tracker.update_progress("d-ota", {"status": "downloading", "progress": 80})
    tracker._progress.clear()
    assert tracker.get_progress("d-ota")["progress"] == 80
    assert "d-ota" in tracker.get_all()

    # Health endpoints check (deep check works; removed mock /run-script returns 404)
    deep = client.get("/api/v1/health", params={"deep": "true"})
    assert deep.status_code == 200
    assert deep.json()["service"] == "esp-fleet-hub"
    assert deep.json()["database"] == "ok"
    assert client.get("/api/v1/health/run-script").status_code == 404
