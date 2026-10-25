from sqlalchemy import create_engine, event
from sqlalchemy.orm import sessionmaker
from app.core.config.config import settings
from app.database.base_class import Base  # 统一的 Base，所有模型必须用这个

# 1. 引擎
connect_args = {}
_is_sqlite = "sqlite" in settings.SQLALCHEMY_DATABASE_URI
if _is_sqlite:
    connect_args["check_same_thread"] = False

engine = create_engine(
    settings.SQLALCHEMY_DATABASE_URI,
    connect_args=connect_args,
    pool_pre_ping=True,
    echo=False,
)

if _is_sqlite:
    @event.listens_for(engine, "connect")
    def _sqlite_tune(dbapi_conn, _record):
        """WAL + synchronous=NORMAL：commit 只追加 WAL 文件、不再逐次 fsync 主库，
        单次提交从 1~15ms 降到微秒级（action_log 高频写入的关键优化）；
        WAL 同时消除读写互斥，查询不再被写事务阻塞。"""
        cursor = dbapi_conn.cursor()
        cursor.execute("PRAGMA journal_mode=WAL")
        cursor.execute("PRAGMA synchronous=NORMAL")
        cursor.close()

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
