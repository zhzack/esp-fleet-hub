from fastapi import APIRouter, Depends, Query
from sqlalchemy.orm import Session
from typing import List
from app.core.security import get_current_user
from app.database.models.user import User
from app.database.session import get_db
from app.database.models.vehicle import VehicleModel
from app.database.schemas.vehicle import VehicleOut

router = APIRouter()


@router.get("", response_model=List[VehicleOut])
@router.get("/", response_model=List[VehicleOut])
def list_vehicles(
    chip: str = Query(None),
    db: Session = Depends(get_db),
    user: User = Depends(get_current_user),
):
    query = db.query(VehicleModel)
    if chip:
        query = query.filter(VehicleModel.chip_platform == chip)
    return query.all()
