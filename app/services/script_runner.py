import asyncio
import os
import subprocess
import threading

from app.services.log_bus import log_bus
from app.services.script_config import ScriptConfig


def run_script_async(
    config: ScriptConfig,
    execution_id: str,
    loop: asyncio.AbstractEventLoop | None = None,
):
    """异步执行脚本，把 stdout/stderr 安全投递到运行中的事件循环 log_bus。"""

    def _emit(line: str | None) -> None:
        if loop is not None and not loop.is_closed():
            asyncio.run_coroutine_threadsafe(log_bus.publish(execution_id, line), loop)
        else:
            asyncio.run(log_bus.publish(execution_id, line))

    def target():
        suffix = config.script_path.suffix.lower()
        if suffix == ".py":
            cmd = [config.interpreter or "python", str(config.script_path)]
        elif suffix == ".sh":
            cmd = [config.interpreter or "bash", str(config.script_path)]
        elif suffix in (".bat", ".cmd"):
            cmd = [str(config.script_path)]
        else:
            _emit(f"[ERROR] Unsupported script type: {suffix}")
            _emit(None)
            return

        if config.args:
            cmd.extend(config.args)

        try:
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                cwd=str(config.workdir),
                env={**os.environ, **config.env},
                text=True,
                bufsize=1,
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                _emit(line.rstrip())
            proc.stdout.close()
            rc = proc.wait()
            _emit(f"[EXIT] code={rc}")
        except Exception as e:
            _emit(f"[ERROR] {e}")
        finally:
            _emit(None)

    threading.Thread(target=target, daemon=True).start()
