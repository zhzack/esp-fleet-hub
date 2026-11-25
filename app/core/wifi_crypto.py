"""凭据对称加密存储。

优先使用独立配置 WIFI_CRYPTO_KEY；未配置时回退至 JWT_SECRET 派生 Fernet key。
支持 `enc:v1:` 版本前缀与历史明文透明读取。
"""
import base64
import hashlib

from cryptography.fernet import Fernet, InvalidToken

from app.core.config.config import settings

_ENC_PREFIX = "enc:v1:"


def _fernet_instance() -> Fernet:
    root = (settings.WIFI_CRYPTO_KEY or "").strip() or settings.JWT_SECRET
    raw = hashlib.sha256(f"wifi-cred:{root}".encode("utf-8")).digest()
    return Fernet(base64.urlsafe_b64encode(raw))


def encrypt(plaintext: str) -> str:
    if plaintext is None:
        return ""
    return _fernet_instance().encrypt(plaintext.encode("utf-8")).decode("utf-8")


def decrypt(token: str) -> str | None:
    if not token:
        return None
    try:
        return _fernet_instance().decrypt(token.encode("utf-8")).decode("utf-8")
    except InvalidToken:
        return None


def encrypt_field(plaintext: str | None) -> str:
    """Encrypt non-empty secret strings with version prefix; avoid double-encryption."""
    if not plaintext:
        return ""
    val = str(plaintext)
    if val.startswith(_ENC_PREFIX):
        return val
    return _ENC_PREFIX + encrypt(val)


def decrypt_field(stored: str | None) -> str:
    """Transparently decrypt `enc:v1:` secrets while staying backward-compatible with legacy plaintext."""
    if not stored:
        return ""
    val = str(stored)
    if val.startswith(_ENC_PREFIX):
        plain = decrypt(val[len(_ENC_PREFIX):])
        return plain if plain is not None else ""
    return val
