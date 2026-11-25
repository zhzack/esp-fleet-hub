from fastapi import APIRouter
from .vehicles import router as vehicles_router
from .actions import router as actions_router
from .commands import router as commands_router
from .slots import router as slots_router
from .host_meta import router as host_meta_router
from .wifi_known import router as wifi_known_router
from .firmwares import router as firmwares_router
from .unified_fleet import router as unified_router

router = APIRouter()
router.include_router(vehicles_router, prefix="/vehicles", tags=["v2-vehicles"])
router.include_router(actions_router, tags=["v2-actions"])
router.include_router(commands_router, prefix="/devices", tags=["v2-devices"])
router.include_router(slots_router, prefix="/devices", tags=["v2-slots"])
router.include_router(host_meta_router, prefix="/devices", tags=["v2-host-meta"])
router.include_router(wifi_known_router, prefix="/devices", tags=["v2-wifi-known"])
router.include_router(firmwares_router, prefix="/firmwares", tags=["v2-firmwares"])
router.include_router(unified_router, prefix="/unified", tags=["v2-unified"])

