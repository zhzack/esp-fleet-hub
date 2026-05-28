from fastapi import APIRouter
from .vehicles import router as vehicles_router
from .actions import router as actions_router
from .commands import router as commands_router

router = APIRouter()
router.include_router(vehicles_router, prefix="/vehicles", tags=["v2-vehicles"])
router.include_router(actions_router, tags=["v2-actions"])
router.include_router(commands_router, prefix="/devices", tags=["v2-devices"])

