from datetime import datetime
from typing import Any, Dict, List, Literal, Optional

from pydantic import BaseModel, Field, model_validator


MacroExecutionMode = Literal["network_debug", "ble_targeted"]


class MacroStep(BaseModel):
    type: str
    key: Optional[str] = None
    content: Optional[str] = None
    x: Optional[int] = None
    y: Optional[int] = None
    button: Optional[int] = None
    delay_ms: Optional[int] = None
    interval_ms: Optional[int] = None


class MacroConfig(BaseModel):
    execution_mode: MacroExecutionMode = "ble_targeted"
    require_connected: bool = True
    action_timeout_ms: int = Field(default=10000, ge=500, le=60000)
    switch_timeout_s: float = Field(default=5.0, ge=0.5, le=60.0)

    @model_validator(mode="after")
    def normalize_for_mode(self):
        if self.execution_mode == "network_debug":
            self.require_connected = False
        return self


class MacroCreate(BaseModel):
    name: str = Field(..., max_length=128)
    steps: List[Dict[str, Any]]
    config: MacroConfig = Field(default_factory=MacroConfig)


class MacroUpdate(BaseModel):
    name: Optional[str] = Field(None, max_length=128)
    steps: Optional[List[Dict[str, Any]]] = None
    config: Optional[MacroConfig] = None


class MacroOut(BaseModel):
    id: int
    user_id: int
    name: str
    content_json: Dict[str, Any]
    created_at: Optional[datetime] = None

    class Config:
        from_attributes = True
