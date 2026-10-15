from datetime import datetime
from typing import Optional, List
from pydantic import BaseModel, ConfigDict


class FirmwareBase(BaseModel):
    version: str
    filename: str
    file_size: int
    sha256: str
    description: Optional[str] = None
    target_chip: str = "esp32s3"


class FirmwareCreate(FirmwareBase):
    file_path: str


class FirmwareOut(FirmwareBase):
    model_config = ConfigDict(from_attributes=True)

    id: int
    created_at: datetime
    download_url: Optional[str] = None


class OtaPushIn(BaseModel):
    device_uuids: List[str]
    custom_url: Optional[str] = None


class OtaProgressOut(BaseModel):
    device_id: str
    status: str  # idle, starting, connecting, downloading, success, failed
    progress: int  # 0 - 100
    bytes_read: int
    total_bytes: int
    running_partition: Optional[str] = None
    target_partition: Optional[str] = None
    version: Optional[str] = None
    error: Optional[str] = None
    updated_at: float
