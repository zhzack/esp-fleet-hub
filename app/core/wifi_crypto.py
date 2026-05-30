"""WiFi 凭据加密存储。

用 JWT_SECRET 派生 Fernet key（HKDF-SHA256）。同一密钥每次启动一致；
JWT_SECRET 更换后所有已存密码不可读，需要用户重新输入。
"""
import base64
import hashlib

from cryptography.fernet import Fernet, InvalidToken

from app.core.config.config import settings


def _key() -> bytes:
    raw = hashlib.sha256(f"wifi-cred:{settings.JWT_SECRET}".encode()).digest()
    return base64.urlsafe_b64encode(raw)


_fernet = Fernet(_key())


def encrypt(plaintext: str) -> str:
    if plaintext is None:
        return ""
    return _fernet.encrypt(plaintext.encode()).decode()


def decrypt(token: str) -> str | None:
    if not token:
        return None
    try:
        return _fernet.decrypt(token.encode()).decode()
    except InvalidToken:
        return None
