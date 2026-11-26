"""SQLAlchemy ORM models for dynamic Home Assistant / MQTT bridge configuration."""

from sqlalchemy import BigInteger, Boolean, Column, Integer, JSON, String, Text

from app.database.base_class import Base


class HaBrokerConfig(Base):
    __tablename__ = "ha_broker_configs"

    id = Column(Integer, primary_key=True, default=1)
    enabled = Column(Boolean, default=False, server_default="0", nullable=False)
    broker_host = Column(String(255), default="", server_default="", nullable=False)
    broker_port = Column(Integer, default=1883, server_default="1883", nullable=False)
    mqtt_user = Column(String(128), default="", server_default="", nullable=False)
    mqtt_pass = Column(Text, default="", server_default="", nullable=False)
    discovery_prefix = Column(String(64), default="homeassistant", server_default="homeassistant", nullable=False)
    topic_prefix = Column(String(64), default="esp_hub", server_default="esp_hub", nullable=False)
    sync_interval_sec = Column(Integer, default=10, server_default="10", nullable=False)
    updated_at = Column(Integer, default=0, server_default="0", nullable=False)


class HaDeviceTarget(Base):
    __tablename__ = "ha_device_targets"

    id = Column(
        BigInteger().with_variant(Integer, "sqlite"),
        primary_key=True,
        autoincrement=True,
    )
    device_uuid = Column(String(128), unique=True, nullable=False, index=True)
    enabled = Column(Boolean, default=True, server_default="1", nullable=False)
    display_name = Column(String(128), default="", server_default="", nullable=False)
    area_name = Column(String(64), default="", server_default="", nullable=False)
    expose_hid = Column(Boolean, default=True, server_default="1", nullable=False)
    expose_gpio = Column(Boolean, default=True, server_default="1", nullable=False)
    expose_display = Column(Boolean, default=True, server_default="1", nullable=False)
    expose_camera = Column(Boolean, default=True, server_default="1", nullable=False)
    pc_enabled = Column(Boolean, default=False, server_default="0", nullable=False)
    pc_name = Column(String(128), default="", server_default="", nullable=False)
    pc_ip = Column(String(64), default="", server_default="", nullable=False)
    pc_mac = Column(String(32), default="", server_default="", nullable=False)
    pc_broadcast_ip = Column(String(64), default="255.255.255.255", server_default="255.255.255.255", nullable=False)
    pc_power_pin = Column(Integer, nullable=True)
    pc_power_pulse_ms = Column(Integer, default=500, server_default="500", nullable=False)
    pc_os = Column(String(16), default="windows", server_default="windows", nullable=False)
    legacy_topic_prefix = Column(String(64), default="", server_default="", nullable=False)
    exposed_pins = Column(JSON, nullable=True)
    updated_at = Column(Integer, default=0, server_default="0", nullable=False)
