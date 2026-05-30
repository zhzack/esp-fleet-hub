from sqlalchemy import (
    Column, BigInteger, Integer, String, TIMESTAMP, DateTime, ForeignKey,
    UniqueConstraint, text,
)
from sqlalchemy.orm import relationship
from app.database.base_class import Base


class DeviceBondSlot(Base):
    """每台 ESP32 设备的"主机槽位"映射。

    设备本身只懂 BLE bond（NimBLE 自管 LTK/IRK），不知道哪个 bond 是"主机 1"。
    本表把 (device, slot_index) → (addr_type, addr) 的映射放服务端，前端按
    "切到 slot 1 / 发送给 slot 2" 这种语义跟用户交互。
    """
    __tablename__ = "device_bond_slots"

    id = Column(BigInteger, primary_key=True, autoincrement=True)
    device_id = Column(
        BigInteger, ForeignKey("devices.id", ondelete="CASCADE"), nullable=False, index=True
    )
    slot_index = Column(Integer, nullable=False)        # 1..N
    addr_type = Column(Integer, nullable=False)
    addr = Column(String(20), nullable=False)           # "AA:BB:CC:DD:EE:FF"
    name = Column(String(64), nullable=True)            # "MacBook-Pro"
    last_used_at = Column(DateTime, nullable=True)
    created_at = Column(TIMESTAMP, server_default=text("CURRENT_TIMESTAMP"))
    updated_at = Column(
        TIMESTAMP,
        server_default=text("CURRENT_TIMESTAMP"),
        server_onupdate=text("CURRENT_TIMESTAMP"),
    )

    device = relationship("Device")

    __table_args__ = (
        UniqueConstraint("device_id", "slot_index", name="uq_device_slot"),
    )
