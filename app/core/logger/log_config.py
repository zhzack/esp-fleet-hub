# app/core/log_config.py
LOG_PIPELINE = {
    "default": {
        "level": "INFO",
        "callbacks": ["console", "file"],
    },
    "error": {
        "level": "ERROR",
        "callbacks": ["file", "http", "alert"],
    },
    "debug": {
        "level": "DEBUG",
        "callbacks": ["console"],
        "only_below": "INFO",  # 只放行 DEBUG 级，避免与 default 管道重复打印
    },
}
