from fastapi import APIRouter, Depends, HTTPException, Request, status
from sqlalchemy.orm import Session

from app.database.session import get_db
from app.database.crud import user as crud_user
from app.database.schemas.user import UserCreate, UserOut, Token, PasswordChange
from app.core.security import create_access_token, get_current_user, verify_password
from app.database.models.user import User

router = APIRouter()


@router.post("/register", response_model=UserOut, status_code=201)
def register(payload: UserCreate, db: Session = Depends(get_db)):
    if crud_user.get_by_username(db, payload.username):
        raise HTTPException(status.HTTP_409_CONFLICT, "用户名已存在，请更换或直接登录")
    user = crud_user.create(db, payload.username, payload.password, payload.role)
    return user


@router.post("/login", response_model=Token)
async def login(request: Request, db: Session = Depends(get_db)):
    username = ""
    password = ""
    content_type = request.headers.get("content-type", "").lower()

    if "application/json" in content_type:
        try:
            body = await request.json()
            username = str(body.get("username", "")).strip()
            password = str(body.get("password", ""))
        except Exception:
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "无效的 JSON 请求体")
    else:
        try:
            form = await request.form()
            username = str(form.get("username", "")).strip()
            password = str(form.get("password", ""))
        except Exception:
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "无效的表单数据")

    if not username or not password:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "用户名和密码不能为空")

    user = crud_user.authenticate(db, username, password)
    if not user:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "用户名或密码错误")

    token, expires_in = create_access_token(sub=str(user.id), role=user.role)
    return Token(access_token=token, expires_in=expires_in)


@router.get("/me", response_model=UserOut)
def me(user: User = Depends(get_current_user)):
    return user


@router.post("/change-password")
def change_password(
    payload: PasswordChange,
    current_user: User = Depends(get_current_user),
    db: Session = Depends(get_db),
):
    if not verify_password(payload.old_password, current_user.password_hash):
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "原密码不正确")
    crud_user.update_password(db, current_user, payload.new_password)
    return {"message": "密码修改成功"}

