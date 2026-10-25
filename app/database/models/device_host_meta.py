"""DeviceHostMeta —— 主机平台标记（按蓝牙地址关联）。

设备侧（固件）无法可靠探测主机 OS（USB HID 主机不申报系统，BLE 配对
信息无 OS 标识），由用户在前端标记；按 (device, addr) 存储，供宏的
平台变体解析、控制台快捷键/组合键适配使用。
"""
from sqlalchemy import (
    BigInteger, Column, DateTime, ForeignKey, Integer, String,
    UniqueConstraint, func,
)
from sqlalchemy.dialects.mysql import BIGINT

from app.database.base_class import Base


class DeviceHostMeta(Base):
    __tablename__ = "device_host_meta"
    __table_args__ = (
        UniqueConstraint("device_id", "addr", name="uq_host_meta_device_addr"),
    )

    id = Column(
        BigInteger().with_variant(Integer, "sqlite"),
        primary_key=True,
        autoincrement=True,
    )
    device_id = Column(
        BigInteger().with_variant(BIGINT, "mysql"),
        ForeignKey("devices.id", ondelete="CASCADE"),
        nullable=False,
        index=True,
    )
    addr = Column(String(20), nullable=False)      # "AA:BB:CC:DD:EE:FF"（大写）
    addr_type = Column(Integer, nullable=False, default=0)
    name = Column(String(64), nullable=True)        # 自定义主机名（可选）
    # ''=未标记 / win / mac / linux
    platform = Column(String(16), nullable=False, default="")
    updated_at = Column(DateTime, nullable=False, server_default=func.now(), onupdate=func.now())
