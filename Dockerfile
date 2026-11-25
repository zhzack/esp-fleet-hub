# syntax=docker/dockerfile:1.6

# ---------- Stage 1: build Vue frontend ----------
FROM node:20-alpine AS frontend
WORKDIR /web
COPY vue-project/package*.json ./
RUN npm ci --no-audit --no-fund || npm install --no-audit --no-fund
COPY vue-project/ ./
RUN npm run build

# ---------- Stage 2: python runtime ----------
FROM python:3.12-slim AS runtime

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1 \
    TZ=Asia/Shanghai

WORKDIR /app

# system deps: curl for healthcheck, openssh-client for remote NAS idf-build
RUN apt-get update \
 && apt-get install -y --no-install-recommends -o Acquire::Retries=3 curl openssh-client \
 && rm -rf /var/lib/apt/lists/*

COPY requirements.txt ./
RUN pip install --upgrade pip \
 && pip install -r requirements.txt \
 && pip install 'bcrypt<4.1'

# app source & unified framework tools
COPY app/ ./app/
COPY alembic.ini ./alembic.ini
COPY alembic/ ./alembic/
COPY esp_unified_framework/ ./esp_unified_framework/
COPY docker-entrypoint.sh ./docker-entrypoint.sh
RUN sed -i 's/\r$//' /app/docker-entrypoint.sh && chmod +x /app/docker-entrypoint.sh

# built frontend from stage 1 -> served by FastAPI (vue-project/dist)
COPY --from=frontend /web/dist/ ./vue-project/dist/

EXPOSE 8000

HEALTHCHECK --interval=15s --timeout=5s --start-period=20s --retries=5 \
  CMD curl -fsS http://127.0.0.1:8000/api/v1/health || exit 1

ENTRYPOINT ["/app/docker-entrypoint.sh"]
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
