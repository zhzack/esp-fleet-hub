"""device_wifi_known CRUD。"""
from datetime import datetime

from sqlalchemy.orm import Session

from app.core.wifi_crypto import encrypt, decrypt
from app.database.models.device_wifi_known import DeviceWifiKnown


def list_for_device(db: Session, device_id: int) -> list[DeviceWifiKnown]:
    # MySQL 不支持 NULLS LAST 语法；用 (last_seen_at IS NULL) 把 NULL 排到末尾
    return (
        db.query(DeviceWifiKnown)
        .filter(DeviceWifiKnown.device_id == device_id)
        .order_by(
            DeviceWifiKnown.last_seen_at.is_(None),   # NULL 行放后面
            DeviceWifiKnown.last_seen_at.desc(),
            DeviceWifiKnown.id.desc(),
        )
        .all()
    )


def get(db: Session, device_id: int, ssid: str) -> DeviceWifiKnown | None:
    return (
        db.query(DeviceWifiKnown)
        .filter(DeviceWifiKnown.device_id == device_id,
                DeviceWifiKnown.ssid == ssid)
        .first()
    )


def upsert(db: Session, device_id: int, ssid: str,
           password: str | None = None, label: str | None = None) -> DeviceWifiKnown:
    row = get(db, device_id, ssid)
    enc = encrypt(password) if password else None
    if row is None:
        row = DeviceWifiKnown(
            device_id=device_id,
            ssid=ssid,
            password_enc=enc,
            label=label,
            last_seen_at=datetime.utcnow(),
        )
        db.add(row)
    else:
        if enc is not None:
            row.password_enc = enc
        if label is not None:
            row.label = label
        row.last_seen_at = datetime.utcnow()
    db.commit()
    db.refresh(row)
    return row


def delete(db: Session, device_id: int, ssid: str) -> bool:
    row = get(db, device_id, ssid)
    if not row:
        return False
    db.delete(row)
    db.commit()
    return True


def reveal_password(row: DeviceWifiKnown) -> str | None:
    """解密密码。仅在确认调用方有权限后才使用。"""
    if not row or not row.password_enc:
        return None
    return decrypt(row.password_enc)
