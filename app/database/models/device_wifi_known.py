"""device_wifi_known：每台 ESP32 见过/保存过的 WiFi 凭据。

密码以 Fernet 加密形式存（详见 app/core/wifi_crypto.py）。如不需要持久化密码，
保持 password_enc 为空，回放时让用户重新输入。
"""
from sqlalchemy import (
    Column, BigInteger, Integer, String, Text, TIMESTAMP, DateTime, ForeignKey,
    UniqueConstraint, text,
)
from sqlalchemy.orm import relationship
from app.database.base_class import Base


class DeviceWifiKnown(Base):
    __tablename__ = "device_wifi_known"

    id = Column(
        BigInteger().with_variant(Integer, "sqlite"),
        primary_key=True,
        autoincrement=True,
    )
    device_id = Column(
        BigInteger, ForeignKey("devices.id", ondelete="CASCADE"), nullable=False, index=True
    )
    ssid = Column(String(64), nullable=False)
    password_enc = Column(Text, nullable=True)        # Fernet ciphertext
    label = Column(String(64), nullable=True)
    last_seen_at = Column(DateTime, nullable=True)
    created_at = Column(TIMESTAMP, server_default=text("CURRENT_TIMESTAMP"))

    device = relationship("Device")

    __table_args__ = (
        UniqueConstraint("device_id", "ssid", name="uq_device_ssid"),
    )
