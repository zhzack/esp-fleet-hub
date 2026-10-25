from datetime import datetime
from typing import Any, Dict, List, Literal, Optional

from pydantic import BaseModel, ConfigDict, Field, model_validator


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


MacroVariantPlatform = Literal["win", "mac", "linux"]


class MacroCreate(BaseModel):
    name: str = Field(..., max_length=128)
    steps: List[Dict[str, Any]]
    config: MacroConfig = Field(default_factory=MacroConfig)
    # 平台变体：{win: [steps], mac: [steps], linux: [steps]}，键为平台名。
    # 运行时按目标主机的平台标记选取，无匹配变体或变体为空则回退通用 steps。
    variants: Optional[Dict[str, List[Dict[str, Any]]]] = None


class MacroUpdate(BaseModel):
    name: Optional[str] = Field(None, max_length=128)
    steps: Optional[List[Dict[str, Any]]] = None
    config: Optional[MacroConfig] = None
    variants: Optional[Dict[str, List[Dict[str, Any]]]] = None


class MacroOut(BaseModel):
    id: int
    user_id: int
    name: str
    content_json: Dict[str, Any]
    created_at: Optional[datetime] = None

    model_config = ConfigDict(from_attributes=True)
