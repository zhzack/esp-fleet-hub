from pydantic_settings import BaseSettings, SettingsConfigDict
from pydantic import Field, field_validator
from typing import List, Union


class Settings(BaseSettings):
    # --- 基础 ---
    PROJECT_NAME: str = "ESP Fleet Hub"
    VERSION: str = "1.0.0"
    DEBUG: bool = Field(default=False)

    # --- MySQL ---
    MYSQL_SERVER: str = Field(default="127.0.0.1")
    MYSQL_PORT: int = Field(default=3306)
    MYSQL_USER: str = Field(default="root")
    MYSQL_PASSWORD: str = Field(default="")
    MYSQL_DB: str = Field(default="esp_fleet_hub")

    # --- Redis ---
    REDIS_HOST: str = Field(default="127.0.0.1")
    REDIS_PORT: int = Field(default=6379)
    REDIS_DB: int = Field(default=0)

    # --- JWT & 加密 ---
    JWT_SECRET: str = Field(default="change-me-please")
    JWT_ALGORITHM: str = Field(default="HS256")
    JWT_EXPIRE_MINUTES: int = Field(default=1440)
    WIFI_CRYPTO_KEY: str = Field(default="")
    ALLOW_SELF_REGISTER: bool = Field(default=True)
    ALLOW_ANONYMOUS_FIRMWARE_DOWNLOAD: bool = Field(default=False)
    ALLOW_UNCLAIMED_DEVICE_CONTROL: bool = Field(default=True)

    # --- 设备 WS / Fleet HTTP 鉴权 ---
    # 为空 = 鉴权关闭（兼容旧部署）；非空 = 设备注册须完成挑战应答。
    DEVICE_SHARED_SECRET: str = Field(default="")
    FLEET_STRICT_DEVICE_AUTH: bool = Field(default=False)

    # --- 队列后端：memory | redis ---
    QUEUE_BACKEND: str = Field(default="memory")

    # --- 远端固件编译流水线 & Hub 外网可达地址（可选环境变量覆盖） ---
    IDF_BUILD_CMD: str = Field(default="idf-build")
    IDF_BUILD_SSH_HOST: str = Field(default="b1ue@192.168.68.146")
    IDF_REMOTE_PROJECT_DIR: str = Field(default="/vol1/esp/projects/esp_unified_framework")
    IDF_SSH_KEY: str = Field(default="~/.ssh/id_ed25519")
    IDF_SSH_STRICT_HOST_KEY: str = Field(default="accept-new")
    HUB_PUBLIC_URL: str = Field(default="")

    # --- 跨域 ---
    BACKEND_CORS_ORIGINS: Union[List[str], str] = []
    BACKEND_CORS_ORIGIN_REGEX: str = Field(default="")

    @field_validator("BACKEND_CORS_ORIGINS", mode="before")
    @classmethod
    def _assemble_cors(cls, v):
        if isinstance(v, str) and not v.startswith("["):
            return [i.strip() for i in v.split(",") if i.strip()]
        return v

    DATABASE_URL: str = Field(default="")

    @property
    def SQLALCHEMY_DATABASE_URI(self) -> str:
        if self.DATABASE_URL:
            return self.DATABASE_URL
        return (
            f"mysql+pymysql://{self.MYSQL_USER}:{self.MYSQL_PASSWORD}"
            f"@{self.MYSQL_SERVER}:{self.MYSQL_PORT}/{self.MYSQL_DB}?charset=utf8mb4"
        )

    @property
    def REDIS_URL(self) -> str:
        return f"redis://{self.REDIS_HOST}:{self.REDIS_PORT}/{self.REDIS_DB}"

    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        case_sensitive=True,
        extra="ignore",
    )


settings = Settings()

_WEAK_SECRETS = {
    "",
    "change-me",
    "change-me-please",
    "change-me-to-a-long-random-string",
    "secret",
}


def validate_runtime_security(*, force: bool = False) -> None:
    """Reject insecure default JWT_SECRET when running in production (DEBUG=False)."""
    import sys

    if not force and (settings.DEBUG or "pytest" in sys.modules):
        return
    sec = (settings.JWT_SECRET or "").strip()
    if sec in _WEAK_SECRETS or len(sec) < 16:
        raise RuntimeError(
            "Unsafe JWT_SECRET configured for non-debug mode; "
            "set a >=16-char random JWT_SECRET in .env or environment."
        )
