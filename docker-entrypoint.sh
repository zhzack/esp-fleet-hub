#!/bin/sh
set -eu

if [ "${SKIP_ALEMBIC_UPGRADE:-0}" != "1" ]; then
  echo "[entrypoint] Running Alembic migrations (alembic upgrade head)..."
  alembic upgrade head
fi

exec "$@"
