"""Pytest adapter executing esp_unified_framework unit tests within the main test suite."""

import sys
from pathlib import Path

import pytest

TOOLS_DIR = Path(__file__).resolve().parents[1] / "esp_unified_framework" / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from test_phase3_hub import TestBuildFlavor, TestFleetHubDb, TestNvsGenerator  # noqa: F401,E402


@pytest.fixture(autouse=True)
def _restore_sql_hub_backend():
    yield
    from app.services import unified_bridge as ub

    ub.hub_db.set_backend(ub.sql_hub_store)
