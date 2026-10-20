# app/core/logger.py
import logging
from .log_callbacks.handler import CallbackHandler
from .log_callbacks.registry import CALLBACK_REGISTRY
from .log_config import LOG_PIPELINE

logger = logging.getLogger("app")
logger.setLevel(logging.DEBUG)

for conf in LOG_PIPELINE.values():
    callbacks = [CALLBACK_REGISTRY[name] for name in conf["callbacks"]]
    handler = CallbackHandler(callbacks)
    handler.setLevel(getattr(logging, conf["level"]))
    # only_below：仅放行低于该级别的记录（如 debug 管道只收 DEBUG，
    # 避免 INFO+ 记录同时命中 default 与 debug 两个 handler 导致控制台重复输出）
    if "only_below" in conf:
        ceiling = getattr(logging, conf["only_below"])
        handler.addFilter(lambda record, c=ceiling: record.levelno < c)
    logger.addHandler(handler)
