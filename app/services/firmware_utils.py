"""Shared OTA URL construction, firmware download authorization, and chip compatibility helpers."""

from __future__ import annotations

import socket
import urllib.parse

from fastapi import HTTPException, Request

from app.core.config.config import settings
from app.core.security import create_download_token, verify_download_token
from app.database.models.user import User


def get_lan_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def resolve_public_base_url(request: Request) -> str:
    """Resolve Hub HTTP base URL reachable by LAN ESP devices, honoring HUB_PUBLIC_URL first."""
    pub = settings.HUB_PUBLIC_URL.strip().rstrip("/")
    if pub:
        return pub
    host = (request.url.hostname or "").lower()
    if host and host not in ("127.0.0.1", "localhost", "::1", "testserver"):
        return f"{request.url.scheme}://{request.url.netloc}"
    port = int(request.url.port) if request.url.port else 8000
    return f"http://{get_lan_ip()}:{port}"


def build_signed_download_url(request: Request, path: str, resource: str, query: dict[str, str] | None = None) -> str:
    """Build Hub firmware download URL carrying a short-lived HMAC signature (`?sig=...`)."""
    base = resolve_public_base_url(request)
    params = dict(query or {})
    params["sig"] = create_download_token(resource)
    return f"{base}{path}?{urllib.parse.urlencode(params)}"


def require_download_auth(resource: str, sig: str | None, user: User | None) -> None:
    """Allow download when authenticated, bearing valid HMAC `?sig=`, or explicitly configured public."""
    if user is not None:
        return
    if verify_download_token(resource, sig):
        return
    if settings.ALLOW_ANONYMOUS_FIRMWARE_DOWNLOAD:
        return
    raise HTTPException(401, "Missing authentication or valid firmware download signature")


def check_chip_compatibility(device_chip: str, fw_target: str) -> tuple[bool, str]:
    """Ensure firmware SoC target matches device chip family when both are known."""
    dev_c = (device_chip or "").lower().replace("-", "")
    fw_t = (fw_target or "").lower().replace("-", "")
    if not dev_c or not fw_t:
        return True, ""
    for family in ("esp8266", "esp32c3", "esp32s3", "esp32c6"):
        if family in dev_c and fw_t != family:
            return False, f"Target mismatch: device={device_chip} vs fw={fw_target}"
    return True, ""
