from pathlib import Path

# 项目根目录（py_web）
BASE_DIR = Path(__file__).resolve().parent.parent

# 静态与脚本资源路径
STATIC_DIR = BASE_DIR / "resource" / "static"
SCRIPT_DIR = BASE_DIR / "resource" / "script"

# 日志目录
LOG_DIR = BASE_DIR.parent / "logs"
EXECUTION_LOG_DIR = LOG_DIR / "executions"

EXECUTION_LOG_DIR.mkdir(parents=True, exist_ok=True)
