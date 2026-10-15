import time
from typing import Dict, Any, Optional


class OtaTracker:
    def __init__(self):
        self._progress: Dict[str, Dict[str, Any]] = {}

    def update_progress(self, device_id: str, payload: Dict[str, Any]) -> None:
        if not device_id:
            return
        now = time.time()
        self._progress[device_id] = {
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

    def get_progress(self, device_id: str) -> Optional[Dict[str, Any]]:
        return self._progress.get(device_id)

    def get_all(self) -> Dict[str, Dict[str, Any]]:
        return dict(self._progress)

    def clear(self, device_id: str) -> None:
        self._progress.pop(device_id, None)


ota_tracker = OtaTracker()
