import json
import time
from typing import Any, Dict, Optional

from app.core.config.config import settings


class OtaTracker:
    def __init__(self):
        self._progress: Dict[str, Dict[str, Any]] = {}
        self._sync_redis = None

    def _redis_client(self):
        if (settings.QUEUE_BACKEND or "memory").lower() != "redis":
            return None
        if self._sync_redis is None:
            try:
                import redis
                self._sync_redis = redis.from_url(settings.REDIS_URL, decode_responses=True)
            except Exception:
                return None
        return self._sync_redis

    def update_progress(self, device_id: str, payload: Dict[str, Any]) -> None:
        if not device_id:
            return
        now = time.time()
        entry = {
            "device_id": device_id,
            "status": payload.get("status", "unknown"),
            "progress": int(payload.get("progress", 0)),
            "bytes_read": int(payload.get("bytes_read", 0)),
            "total_bytes": int(payload.get("total_bytes", 0)),
            "running_partition": payload.get("running_partition"),
            "target_partition": payload.get("target_partition"),
            "version": payload.get("version"),
            "error": payload.get("error"),
            "updated_at": now,
        }
        self._progress[device_id] = entry
        cli = self._redis_client()
        if cli is not None:
            try:
                cli.hset("ota:progress:all", device_id, json.dumps(entry))
            except Exception:
                pass

    def get_progress(self, device_id: str) -> Optional[Dict[str, Any]]:
        val = self._progress.get(device_id)
        if val is not None:
            return val
        cli = self._redis_client()
        if cli is not None:
            try:
                raw = cli.hget("ota:progress:all", device_id)
                if raw:
                    parsed = json.loads(raw)
                    self._progress[device_id] = parsed
                    return parsed
            except Exception:
                pass
        return None

    def get_all(self) -> Dict[str, Dict[str, Any]]:
        cli = self._redis_client()
        if cli is not None:
            try:
                raw_map = cli.hgetall("ota:progress:all")
                for k, v in raw_map.items():
                    self._progress.setdefault(k, json.loads(v))
            except Exception:
                pass
        return dict(self._progress)

    def clear(self, device_id: str) -> None:
        self._progress.pop(device_id, None)
        cli = self._redis_client()
        if cli is not None:
            try:
                cli.hdel("ota:progress:all", device_id)
            except Exception:
                pass


ota_tracker = OtaTracker()
