from datetime import datetime, timezone


def ping() -> dict:
    return {
        "status": "ok",
        "service": "esp-fleet-hub",
        "timestamp": datetime.now(timezone.utc).isoformat()
    }


def ping2() -> dict:
    return {
        "status": "ok",
        "service": "esp-fleet-hub",
        "timestamp": datetime.now(timezone.utc).isoformat()
    }
