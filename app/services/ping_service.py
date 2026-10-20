from datetime import datetime, timezone


def ping() -> dict:
    return {
        "status": "ok",
        "service": "minimal-web-demo",
        "timestamp": datetime.now(timezone.utc).isoformat()
    }


def ping2() -> dict:
    return {
        "status": "ok",
        "service": "minimal-web-demo",
        "timestamp": datetime.now(timezone.utc).isoformat()
    }
