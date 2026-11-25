"""Bridge module connecting py_web FastAPI with esp_unified_framework tools."""

from __future__ import annotations

import os
import shutil
import sys
import threading
import uuid
from pathlib import Path
from typing import Optional

from sqlalchemy.orm import Session

from app.core.config.config import settings
from app.core.logger.logger import logger
from app.database.crud import firmware as crud_firmware
from app.database.schemas.firmware import FirmwareCreate
from app.services.unified_store import migrate_legacy_fleet_sqlite, sql_hub_store

UNIFIED_ROOT = Path(__file__).resolve().parents[2] / "esp_unified_framework"
TOOLS_DIR = UNIFIED_ROOT / "tools"
HID_UPLOAD_DIR = Path("uploads/firmwares")

if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import build_flavor  # noqa: E402
import fleet_router  # noqa: E402
import hub_db  # noqa: E402
import mdns_advertiser  # noqa: E402
import web_compiler  # noqa: E402

# Route unified fleet operations into py_web's unified SQLAlchemy database
hub_db.set_backend(sql_hub_store)

_mdns_adv: Optional[mdns_advertiser.MdnsHubAdvertiser] = None


def init_unified_hub():
    hub_db.set_backend(sql_hub_store)
    os.environ.setdefault("IDF_BUILD_CMD", settings.IDF_BUILD_CMD)
    os.environ.setdefault("IDF_BUILD_SSH_HOST", settings.IDF_BUILD_SSH_HOST)
    os.environ.setdefault("IDF_REMOTE_PROJECT_DIR", settings.IDF_REMOTE_PROJECT_DIR)
    os.environ.setdefault("IDF_SSH_KEY", settings.IDF_SSH_KEY)
    os.environ.setdefault("IDF_SSH_STRICT_HOST_KEY", settings.IDF_SSH_STRICT_HOST_KEY)
    if "pytest" not in sys.modules:
        sql_hub_store.mark_stale_compiler_tasks_failed()
    web_compiler.set_task_store(sql_hub_store.save_compiler_task, sql_hub_store.load_compiler_tasks)
    legacy_db = UNIFIED_ROOT / "data" / "fleet.db"
    if "pytest" not in sys.modules and legacy_db.is_file():
        moved = migrate_legacy_fleet_sqlite(str(legacy_db))
        if moved:
            logger.info(f"Migrated {moved} legacy device(s) from fleet.db into primary DB")


def start_mdns_hub(port: int = 8000):
    global _mdns_adv
    init_unified_hub()
    if _mdns_adv is not None:
        return
    try:
        _mdns_adv = mdns_advertiser.MdnsHubAdvertiser(port=port)
        _mdns_adv.start()
        lan_ip = mdns_advertiser.get_lan_ip()
        logger.info(f"📡 Unified mDNS Hub active: http://{lan_ip}:{port} (_esp-hub._tcp)")
    except Exception as e:
        logger.warning(f"Unified mDNS startup skipped: {e}")


def stop_mdns_hub():
    global _mdns_adv
    if _mdns_adv is not None:
        try:
            _mdns_adv.stop()
        except Exception:
            pass
        _mdns_adv = None


def lookup_device_ws_secret(device_uuid: str) -> str:
    """Return pre-provisioned hub_db secret for a device if present."""
    if not device_uuid:
        return ""
    try:
        hub_db.ensure_init()
        dev = hub_db.get_device(device_uuid)
        if dev and dev.get("device_secret"):
            return str(dev["device_secret"])
    except Exception:
        pass
    return ""


def start_cloud_build(payload: dict, hub_port: int = 8000) -> str:
    target = payload.get("target", "esp32")
    features = payload.get("features", {})
    hardware = payload.get("hardware", {})
    build_flavor.validate_soc_compatibility(target, features, hardware)
    profile_name = payload.get("name", "custom_build")
    tid = web_compiler.create_task("build", f"{profile_name} ({target})", status="building")
    t = threading.Thread(
        target=web_compiler.run_build_task,
        args=(payload,),
        kwargs={"hub_port": hub_port, "task_id": tid},
        daemon=True,
    )
    t.start()
    return tid


def start_serial_flash(payload: dict, hub_port: int = 8000) -> str:
    port = payload.get("port", "COM11")
    fn = payload.get("file", "")
    tid = web_compiler.create_task("flash", f"{port} <- {fn}", status="flashing")
    t = threading.Thread(
        target=web_compiler.run_serial_flash_task,
        args=(
            port,
            int(payload.get("baud", 921600)),
            fn,
            payload.get("target", "esp32"),
            payload.get("full", True),
            payload.get("auto_provision", True),
            payload.get("wifi_ssid", "1"),
            payload.get("wifi_pass", "11111111"),
            payload.get("mdns_host", "esp-node"),
        ),
        kwargs={"hub_port": hub_port, "task_id": tid},
        daemon=True,
    )
    t.start()
    return tid


def publish_firmware_to_hid_repo(
    db: Session,
    fw_file: str,
    version_override: Optional[str] = None,
    description: Optional[str] = None,
):
    src = Path(build_flavor.DIST_DIR) / fw_file
    if not src.is_file():
        raise FileNotFoundError(f"Unified firmware not found: {fw_file}")

    meta = build_flavor.load_artifact_meta(fw_file) or {}
    sha256 = meta.get("sha256") or build_flavor.compute_sha256(str(src))
    size = src.stat().st_size
    version = version_override or meta.get("fw_version", "1.1.0-unified")
    target_chip = meta.get("target", "esp32s3")

    HID_UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    safe_name = f"{str(uuid.uuid4())[:8]}_{fw_file}"
    dst = HID_UPLOAD_DIR / safe_name
    shutil.copyfile(src, dst)

    return crud_firmware.create_firmware(
        db,
        FirmwareCreate(
            version=version,
            filename=fw_file,
            file_path=str(dst),
            file_size=size,
            sha256=sha256,
            description=description or f"Published from Unified Framework ({target_chip})",
            target_chip=target_chip,
        ),
    )
