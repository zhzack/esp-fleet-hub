"""msg_id 幂等去重（支持内存 LRU 与可选 Redis SET NX EX）。"""
from collections import OrderedDict
import threading

from app.core.config.config import settings

_MAX = 2000
_seen: "OrderedDict[str, int]" = OrderedDict()
_lock = threading.Lock()


def seen_or_record(msg_id: str) -> bool:
    """如果之前见过返回 True；否则记录并返回 False。"""
    if not msg_id:
        return False
    with _lock:
        if msg_id in _seen:
            _seen.move_to_end(msg_id)
            return True
        _seen[msg_id] = 1
        if len(_seen) > _MAX:
            _seen.popitem(last=False)
        return False


async def seen_or_record_async(msg_id: str, ttl_sec: int = 300) -> bool:
    """Redis 后端启用时使用跨实例 SET NX EX 去重，不可达时回退内存 LRU。"""
    if not msg_id:
        return False
    if (settings.QUEUE_BACKEND or "memory").lower() == "redis":
        try:
            from app.core.cache import get_redis

            inserted = await get_redis().set(f"idemp:{msg_id}", "1", ex=ttl_sec, nx=True)
            return not bool(inserted)
        except Exception:
            pass
    return seen_or_record(msg_id)
