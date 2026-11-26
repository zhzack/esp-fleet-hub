"""Persistent per-device timers and scheduled automation rules."""

from sqlalchemy import (
    BigInteger,
    Boolean,
    Column,
    ForeignKey,
    Integer,
    JSON,
    String,
    TIMESTAMP,
    text,
)
from app.database.base_class import Base


class DeviceTimer(Base):
    __tablename__ = "device_timers"

    id = Column(
        BigInteger().with_variant(Integer, "sqlite"),
        primary_key=True,
        autoincrement=True,
    )
    user_id = Column(
        BigInteger().with_variant(Integer, "sqlite"),
        ForeignKey("users.id", ondelete="SET NULL"),
        nullable=True,
        index=True,
    )
    device_uuid = Column(String(128), nullable=False, index=True)
    name = Column(String(128), nullable=False)
    enabled = Column(Boolean, nullable=False, default=True, server_default="1")

    # countdown | once | daily | interval
    schedule_type = Column(String(24), nullable=False)
    delay_sec = Column(Integer, nullable=False, default=0)
    interval_sec = Column(Integer, nullable=False, default=0)
    time_hhmm = Column(String(8), nullable=False, default="")
    weekdays = Column(JSON, nullable=True)

    # gpio_set | display_config | hid_transport | macro | reboot
    action_type = Column(String(32), nullable=False)
    action_payload = Column(JSON, nullable=False)

    next_run_at = Column(Integer, nullable=True, index=True)
    last_run_at = Column(Integer, nullable=True)
    last_status = Column(String(24), nullable=False, default="pending")
    last_message = Column(String(255), nullable=False, default="")
    run_count = Column(Integer, nullable=False, default=0)

    created_at = Column(TIMESTAMP, server_default=text("CURRENT_TIMESTAMP"))
    updated_at = Column(
        TIMESTAMP,
        server_default=text("CURRENT_TIMESTAMP"),
        server_onupdate=text("CURRENT_TIMESTAMP"),
    )
