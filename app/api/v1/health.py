from fastapi import APIRouter, Depends, HTTPException, Query
from sqlalchemy import text
from sqlalchemy.orm import Session

from app.core.config.config import settings
from app.database.session import get_db
from app.services.ping_service import ping, ping2

router = APIRouter()


@router.get("")
def health_check(
    deep: bool = Query(False),
    db: Session = Depends(get_db),
):
    res = ping()
    if deep:
        try:
            db.execute(text("SELECT 1"))
            res["database"] = "ok"
        except Exception as e:
            raise HTTPException(503, f"database unhealthy: {e}")
        res["queue_backend"] = settings.QUEUE_BACKEND
    return res


@router.get("/ping")
def ping_check():
    return ping2()



