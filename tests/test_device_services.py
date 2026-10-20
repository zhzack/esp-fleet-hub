"""tests/test_device_services.py - ESP32 链路服务组件单元测试

覆盖 dispatcher._parse_ack_payload / AckTracker / OtaTracker / 幂等去重。
"""
import asyncio

import pytest

from app.services.ack_tracker import AckTracker
from app.services.idempotency import seen_or_record
from app.services.ota_tracker import OtaTracker
from app.services.dispatcher import _parse_ack_payload


class TestParseAckPayload:
    def test_status_ok_variants(self):
        assert _parse_ack_payload({"status": "ok", "exec_time_ms": 12}) == (True, 12, None, None)
        assert _parse_ack_payload({"status": "ACKED"}) == (True, 0, None, None)
        assert _parse_ack_payload({"status": "Success"}) == (True, 0, None, None)

    def test_status_failed_with_error_dict(self):
        ok, ms, code, msg = _parse_ack_payload(
            {"status": "failed", "error": {"code": "E101", "message": "hid busy"}}
        )
        assert (ok, ms, code, msg) == (False, 0, "E101", "hid busy")

    def test_ok_key_false_with_flat_error(self):
        ok, ms, code, msg = _parse_ack_payload(
            {"ok": False, "err_code": "E5", "err_msg": "bad param", "exec_time_ms": 3}
        )
        assert (ok, ms, code, msg) == (False, 3, "E5", "bad param")

    def test_flat_error_keys_imply_failure(self):
        ok, _, code, _ = _parse_ack_payload({"error_code": "E777", "error_message": "x"})
        assert ok is False
        assert code == "E777"

    def test_empty_dict_defaults_to_success(self):
        assert _parse_ack_payload({}) == (True, 0, None, None)

    def test_non_dict_is_invalid(self):
        assert _parse_ack_payload("ok") == (False, 0, "E000", "invalid ack payload")


class TestAckTracker:
    def test_expect_resolved_by_ref(self):
        async def scenario():
            tracker = AckTracker()
            fut = asyncio.ensure_future(tracker.expect("m-1", timeout=2))
            await asyncio.sleep(0)  # 让 expect 先注册 Future
            assert tracker.resolve("m-1", {"status": "ok"}) is True
            return await fut

        assert asyncio.run(scenario()) == {"status": "ok"}
        # resolve 后 pending 清空
        tracker = AckTracker()
        assert tracker.pending_count() == 0

    def test_resolve_unknown_ref_returns_false(self):
        tracker = AckTracker()
        assert tracker.resolve("nope", {}) is False

    def test_expect_timeout(self):
        async def scenario():
            tracker = AckTracker()
            await asyncio.wait_for(tracker.expect("m-2", timeout=0.05), timeout=1)

        with pytest.raises(asyncio.TimeoutError):
            asyncio.run(scenario())


class TestOtaTracker:
    def test_update_get_clear(self):
        t = OtaTracker()
        t.update_progress(
            "esp32_ota_001",
            {"status": "downloading", "progress": "75", "bytes_read": 1500,
             "total_bytes": 2000, "version": "1.2.0"},
        )
        p = t.get_progress("esp32_ota_001")
        assert p["status"] == "downloading"
        assert p["progress"] == 75          # 字符串被强转 int
        assert p["bytes_read"] == 1500
        assert p["version"] == "1.2.0"
        assert p["updated_at"] > 0

        assert t.get_progress("unknown") is None
        assert "esp32_ota_001" in t.get_all()

        t.clear("esp32_ota_001")
        assert t.get_progress("esp32_ota_001") is None

    def test_empty_device_id_ignored(self):
        t = OtaTracker()
        t.update_progress("", {"progress": 50})
        assert t.get_all() == {}


class TestIdempotency:
    def test_seen_or_record_semantics(self):
        assert seen_or_record("abc") is False   # 首次：记录并放行
        assert seen_or_record("abc") is True    # 再次：命中去重
        assert seen_or_record("xyz") is False

    def test_empty_msg_id_always_passes(self):
        assert seen_or_record("") is False
        assert seen_or_record("") is False
