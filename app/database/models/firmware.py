from sqlalchemy import Column, BigInteger, Integer, String, TIMESTAMP, text
from app.database.base_class import Base


class Firmware(Base):
    __tablename__ = "firmwares"

    id = Column(
        BigInteger().with_variant(Integer, "sqlite"),
        primary_key=True,
        autoincrement=True,
    )
    version = Column(String(64), nullable=False)
    filename = Column(String(255), nullable=False)
    file_path = Column(String(512), nullable=False)
    file_size = Column(Integer, nullable=False)
    sha256 = Column(String(64), nullable=False)
    description = Column(String(255), nullable=True)
    target_chip = Column(String(64), default="esp32s3")
    created_at = Column(TIMESTAMP, server_default=text("CURRENT_TIMESTAMP"))
