"""JWT 鉴权 + 密码哈希 + 下载签名 + 设备访问控制依赖。"""
from datetime import datetime, timedelta, timezone
import hashlib
import hmac
import time
from typing import Optional

from fastapi import Depends, HTTPException, WebSocket, status
from fastapi.security import OAuth2PasswordBearer
from jose import JWTError, jwt
from sqlalchemy.orm import Session

from app.core.config.config import settings
from app.database.session import get_db
from app.database.models.device import Device
from app.database.models.user import User

import bcrypt


def hash_password(plain: str) -> str:
    pw = plain.encode("utf-8")[:72]
    return bcrypt.hashpw(pw, bcrypt.gensalt()).decode("utf-8")


def verify_password(plain: str, hashed: str) -> bool:
    try:
        return bcrypt.checkpw(plain.encode("utf-8")[:72], hashed.encode("utf-8"))
    except Exception:
        return False


def create_access_token(
    sub: str, role: str = "user", expires_minutes: Optional[int] = None
) -> tuple[str, int]:
    """返回 (token, expires_in_seconds)。"""
    exp_min = expires_minutes or int(settings.JWT_EXPIRE_MINUTES)
    now = datetime.now(timezone.utc)
    exp = now + timedelta(minutes=exp_min)
    payload = {
        "sub": str(sub),
        "role": role,
        "iat": int(now.timestamp()),
        "exp": int(exp.timestamp()),
    }
    token = jwt.encode(payload, settings.JWT_SECRET, algorithm=settings.JWT_ALGORITHM)
    return token, exp_min * 60


def decode_token(token: str) -> dict:
    return jwt.decode(token, settings.JWT_SECRET, algorithms=[settings.JWT_ALGORITHM])


def create_download_token(resource: str, ttl_sec: int = 1800) -> str:
    """Generate compact `<exp_hex>.<hmac24>` signed token for OTA/browser downloads."""
    exp = int(time.time()) + max(1, int(ttl_sec))
    msg = f"{resource}:{exp}".encode("utf-8")
    sig = hmac.new(settings.JWT_SECRET.encode("utf-8"), msg, hashlib.sha256).hexdigest()[:24]
    return f"{exp:x}.{sig}"


def verify_download_token(resource: str, token: str | None) -> bool:
    """Validate compact `<exp_hex>.<hmac24>` download token."""
    if not token or "." not in token:
        return False
    try:
        exp_hex, sig = token.split(".", 1)
        exp = int(exp_hex, 16)
    except (ValueError, TypeError):
        return False
    if exp < int(time.time()):
        return False
    msg = f"{resource}:{exp}".encode("utf-8")
    expected = hmac.new(settings.JWT_SECRET.encode("utf-8"), msg, hashlib.sha256).hexdigest()[:24]
    return hmac.compare_digest(expected, sig)


def ensure_device_access(
    db: Session,
    user: User,
    device_uuid: str,
    *,
    allow_unclaimed: bool | None = None,
) -> Device:
    """Enforce ownership/admin authorization on a primary `devices` row."""
    dev = db.query(Device).filter(Device.device_uuid == device_uuid).first()
    if not dev:
        raise HTTPException(404, "device not found")
    if user.role == "admin" or dev.owner_user_id == user.id:
        return dev
    unclaimed_ok = settings.ALLOW_UNCLAIMED_DEVICE_CONTROL if allow_unclaimed is None else allow_unclaimed
    if dev.owner_user_id is None and unclaimed_ok:
        return dev
    raise HTTPException(403, "not your device")


oauth2_scheme = OAuth2PasswordBearer(tokenUrl="/api/v1/auth/login", auto_error=False)


async def get_current_user(
    token: Optional[str] = Depends(oauth2_scheme),
    db: Session = Depends(get_db),
) -> User:
    if not token:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="missing token",
            headers={"WWW-Authenticate": "Bearer"},
        )
    try:
        payload = decode_token(token)
        user_id = int(payload.get("sub"))
    except (JWTError, ValueError, TypeError):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="invalid token",
            headers={"WWW-Authenticate": "Bearer"},
        )
    user = db.query(User).filter(User.id == user_id).first()
    if not user or not user.is_active:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED, detail="user not found"
        )
    return user


async def get_optional_user(
    token: Optional[str] = Depends(oauth2_scheme),
    db: Session = Depends(get_db),
) -> Optional[User]:
    """Return authenticated active user if Bearer token is valid, else None."""
    if not token:
        return None
    try:
        payload = decode_token(token)
        user_id = int(payload.get("sub"))
    except (JWTError, ValueError, TypeError):
        return None
    user = db.query(User).filter(User.id == user_id).first()
    if not user or not user.is_active:
        return None
    return user


async def get_current_admin(user: User = Depends(get_current_user)) -> User:
    if user.role != "admin":
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="admin only")
    return user


async def ws_authenticate(websocket: WebSocket, db: Session) -> Optional[User]:
    """WebSocket 鉴权：从 query string `token=...` 取 JWT。

    失败时关闭连接并返回 None。
    """
    token = websocket.query_params.get("token")
    if not token:
        await websocket.close(code=4401)
        return None
    try:
        payload = decode_token(token)
        user_id = int(payload.get("sub"))
    except (JWTError, ValueError, TypeError):
        await websocket.close(code=4401)
        return None
    user = db.query(User).filter(User.id == user_id).first()
    if not user or not user.is_active:
        await websocket.close(code=4401)
        return None
    return user
