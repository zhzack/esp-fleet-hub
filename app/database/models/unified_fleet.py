"""SQLAlchemy ORM models for ESP Unified Framework fleet registry, shadow config & logs."""

from sqlalchemy import BigInteger, Column, Integer, String, Text
from app.database.base_class import Base


class UnifiedDevice(Base):
    __tablename__ = "unified_devices"

    id = Column(
        BigInteger().with_variant(Integer, "sqlite"),
        primary_key=True,
        autoincrement=True,
    )
    device_id = Column(String(128), unique=True, nullable=False, index=True)
    mac = Column(String(32), default="", nullable=False)
    name = Column(String(128), default="", nullable=False)
    chip = Column(String(64), default="", nullable=False)
    board = Column(String(64), default="", nullable=False)
    flash_mb = Column(Integer, default=4, nullable=False)
    psram_mb = Column(Integer, default=0, nullable=False)
    ip = Column(String(64), default="", nullable=False)
    mdns_host = Column(String(128), default="", nullable=False)
    web_port = Column(Integer, default=80, nullable=False)
    fw_version = Column(String(64), default="", nullable=False)
    capabilities_json = Column(Text, default="{}", nullable=False)
    status = Column(String(32), default="provisioned", nullable=False)
    claimed = Column(Integer, default=0, nullable=False)
    device_secret = Column(String(128), default="", nullable=False)
    api_token = Column(String(128), default="", nullable=False)
    rssi = Column(Integer, default=0, nullable=False)
    free_heap = Column(Integer, default=0, nullable=False)
    uptime_sec = Column(Integer, default=0, nullable=False)
    last_seen_at = Column(Integer, default=0, nullable=False)
    created_at = Column(Integer, default=0, nullable=False)


class UnifiedDeviceConfig(Base):
    __tablename__ = "unified_device_configs"

    device_id = Column(String(128), primary_key=True)
    desired_json = Column(Text, default="{}", nullable=False)
    reported_json = Column(Text, default="{}", nullable=False)
    desired_ver = Column(Integer, default=1, nullable=False)
    reported_ver = Column(Integer, default=1, nullable=False)
    sync_status = Column(String(32), default="synced", nullable=False)
    updated_at = Column(Integer, default=0, nullable=False)


class UnifiedPendingCommand(Base):
    __tablename__ = "unified_pending_commands"

    id = Column(
        BigInteger().with_variant(Integer, "sqlite"),
        primary_key=True,
        autoincrement=True,
    )
    device_id = Column(String(128), nullable=False, index=True)
    cmd_type = Column(String(64), nullable=False)
    payload_json = Column(Text, default="{}", nullable=False)
    status = Column(String(32), default="queued", nullable=False)
    created_at = Column(Integer, default=0, nullable=False)
    updated_at = Column(Integer, default=0, nullable=False)


class UnifiedAuthNonce(Base):
    __tablename__ = "unified_auth_nonces"

    nonce = Column(String(128), primary_key=True)
    device_id = Column(String(128), default="", nullable=False)
    expires_at = Column(Integer, nullable=False)


class UnifiedAuditLog(Base):
    __tablename__ = "unified_audit_logs"

    id = Column(
        BigInteger().with_variant(Integer, "sqlite"),
        primary_key=True,
        autoincrement=True,
    )
    device_id = Column(String(128), default="", nullable=False, index=True)
    event = Column(String(64), nullable=False)
    detail = Column(Text, default="", nullable=False)
    created_at = Column(Integer, default=0, nullable=False)
