from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker
from app.core.config.config import settings
from app.database.base_class import Base  # 统一的 Base，所有模型必须用这个

# 1. 引擎
connect_args = {}
if "sqlite" in settings.SQLALCHEMY_DATABASE_URI:
    connect_args["check_same_thread"] = False

engine = create_engine(
    settings.SQLALCHEMY_DATABASE_URI,
    connect_args=connect_args,
    pool_pre_ping=True,
    echo=False,
)

# 2. 会话工厂
SessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=engine)

# 3. 暴露 Base，向后兼容旧代码（之前从 session.py 导入）
__all__ = ["engine", "SessionLocal", "Base", "get_db"]


# 4. FastAPI 依赖：获取数据库会话
def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()
