"""主机平台标记（DeviceHostMeta）REST API。

固件无法可靠探测主机 OS，由用户按蓝牙地址标记平台（win/mac/linux），
供宏平台变体解析与控制台快捷键适配使用。
"""
from typing import List, Literal, Optional

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, ConfigDict
from sqlalchemy.orm import Session

from app.core.security import ensure_device_access, get_current_user
from app.database.crud import device_host_meta as crud_meta
from app.database.models.device import Device
from app.database.models.user import User
from app.database.session import get_db

router = APIRouter()


class HostMetaIn(BaseModel):
    platform: Literal["", "win", "mac", "linux"] = ""
    name: Optional[str] = None
    addr_type: int = 0


class HostMetaOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    addr: str
    addr_type: int
    name: Optional[str] = None
    platform: str


@router.get("/{device_uuid}/host-meta", response_model=List[HostMetaOut])
def list_host_meta(
    device_uuid: str,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    dev = ensure_device_access(db, user, device_uuid)
    return crud_meta.list_for_device(db, dev.id)


@router.put("/{device_uuid}/host-meta/{addr}", response_model=HostMetaOut)
def put_host_meta(
    device_uuid: str,
    addr: str,
    body: HostMetaIn,
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    if ":" not in addr or len(addr.split(":")) != 6:
        raise HTTPException(400, "addr must be AA:BB:CC:DD:EE:FF")
    dev = ensure_device_access(db, user, device_uuid)
    return crud_meta.upsert(
        db, dev.id, addr,
        platform=body.platform, name=body.name, addr_type=body.addr_type,
    )
