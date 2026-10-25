from sqlalchemy.orm import Session

from app.database.models.macro import Macro
from app.database.schemas.macro import MacroConfig


def _normalize_content(
    *,
    steps: list[dict],
    config: dict | MacroConfig | None = None,
    variants: dict | None = None,
) -> dict:
    cfg = config if isinstance(config, MacroConfig) else MacroConfig(**(config or {}))
    content: dict = {"steps": steps, "config": cfg.model_dump()}
    # 平台变体：仅保留合法平台键与非空步骤列表，避免脏数据入库
    if variants:
        clean = {}
        for platform, vsteps in (variants or {}).items():
            if platform in ("win", "mac", "linux") and isinstance(vsteps, list) and vsteps:
                clean[platform] = vsteps
        if clean:
            content["variants"] = clean
    return content


def get(db: Session, macro_id: int) -> Macro | None:
    return db.query(Macro).filter(Macro.id == macro_id).first()


def list_for_user(db: Session, user_id: int) -> list[Macro]:
    return (
        db.query(Macro)
        .filter(Macro.user_id == user_id)
        .order_by(Macro.id.desc())
        .all()
    )


def create(
    db: Session,
    user_id: int,
    name: str,
    steps: list[dict],
    config: dict | MacroConfig | None = None,
    variants: dict | None = None,
) -> Macro:
    m = Macro(
        user_id=user_id,
        name=name,
        content_json=_normalize_content(steps=steps, config=config, variants=variants),
    )
    db.add(m)
    db.commit()
    db.refresh(m)
    return m


def update(
    db: Session,
    macro_id: int,
    user_id: int,
    name: str | None = None,
    steps: list[dict] | None = None,
    config: dict | MacroConfig | None = None,
    variants: dict | None = None,
) -> Macro | None:
    m = db.query(Macro).filter(Macro.id == macro_id, Macro.user_id == user_id).first()
    if not m:
        return None
    if name is not None:
        m.name = name
    if steps is not None or config is not None or variants is not None:
        current = m.content_json or {}
        # variants 显式传 {} = 清空全部平台变体；None = 保持不变
        if variants is None:
            variants = current.get("variants")
        m.content_json = _normalize_content(
            steps=steps if steps is not None else list(current.get("steps") or []),
            config=config if config is not None else current.get("config"),
            variants=variants,
        )
    db.commit()
    db.refresh(m)
    return m


def delete(db: Session, macro_id: int, user_id: int) -> bool:
    m = db.query(Macro).filter(Macro.id == macro_id, Macro.user_id == user_id).first()
    if not m:
        return False
    db.delete(m)
    db.commit()
    return True
