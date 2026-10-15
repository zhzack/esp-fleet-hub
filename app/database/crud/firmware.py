from typing import List, Optional
from sqlalchemy.orm import Session
from app.database.models.firmware import Firmware
from app.database.schemas.firmware import FirmwareCreate


def create_firmware(db: Session, obj_in: FirmwareCreate) -> Firmware:
    db_obj = Firmware(
        version=obj_in.version,
        filename=obj_in.filename,
        file_path=obj_in.file_path,
        file_size=obj_in.file_size,
        sha256=obj_in.sha256,
        description=obj_in.description,
        target_chip=obj_in.target_chip,
    )
    db.add(db_obj)
    db.commit()
    db.refresh(db_obj)
    return db_obj


def get_firmwares(db: Session, skip: int = 0, limit: int = 100) -> List[Firmware]:
    return (
        db.query(Firmware)
        .order_by(Firmware.created_at.desc())
        .offset(skip)
        .limit(limit)
        .all()
    )


def get_firmware(db: Session, firmware_id: int) -> Optional[Firmware]:
    return db.query(Firmware).filter(Firmware.id == firmware_id).first()


def delete_firmware(db: Session, firmware_id: int) -> Optional[Firmware]:
    db_obj = db.query(Firmware).filter(Firmware.id == firmware_id).first()
    if db_obj:
        db.delete(db_obj)
        db.commit()
    return db_obj
