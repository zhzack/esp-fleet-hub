"""add_device_timers_table

Revision ID: c7a8e19b2d30
Revises: ba33aadb2f68
Create Date: 2026-11-26 00:42:30.000000
"""
from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa


revision: str = "c7a8e19b2d30"
down_revision: Union[str, Sequence[str], None] = "ba33aadb2f68"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    insp = sa.inspect(op.get_bind())
    if not insp.has_table("device_timers"):
        op.create_table(
            "device_timers",
            sa.Column("id", sa.BigInteger().with_variant(sa.Integer, "sqlite"), autoincrement=True, nullable=False),
            sa.Column("user_id", sa.BigInteger().with_variant(sa.Integer, "sqlite"), nullable=True),
            sa.Column("device_uuid", sa.String(length=128), nullable=False),
            sa.Column("name", sa.String(length=128), nullable=False),
            sa.Column("enabled", sa.Boolean(), server_default="1", nullable=False),
            sa.Column("schedule_type", sa.String(length=24), nullable=False),
            sa.Column("delay_sec", sa.Integer(), nullable=False, server_default="0"),
            sa.Column("interval_sec", sa.Integer(), nullable=False, server_default="0"),
            sa.Column("time_hhmm", sa.String(length=8), nullable=False, server_default=""),
            sa.Column("weekdays", sa.JSON(), nullable=True),
            sa.Column("action_type", sa.String(length=32), nullable=False),
            sa.Column("action_payload", sa.JSON(), nullable=False),
            sa.Column("next_run_at", sa.Integer(), nullable=True),
            sa.Column("last_run_at", sa.Integer(), nullable=True),
            sa.Column("last_status", sa.String(length=24), nullable=False, server_default="pending"),
            sa.Column("last_message", sa.String(length=255), nullable=False, server_default=""),
            sa.Column("run_count", sa.Integer(), nullable=False, server_default="0"),
            sa.Column("created_at", sa.TIMESTAMP(), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=True),
            sa.Column("updated_at", sa.TIMESTAMP(), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=True),
            sa.ForeignKeyConstraint(["user_id"], ["users.id"], ondelete="SET NULL"),
            sa.PrimaryKeyConstraint("id"),
        )
    existing_idx = {idx["name"] for idx in sa.inspect(op.get_bind()).get_indexes("device_timers")}
    with op.batch_alter_table("device_timers", schema=None) as batch_op:
        for idx_name, cols in (
            (batch_op.f("ix_device_timers_device_uuid"), ["device_uuid"]),
            (batch_op.f("ix_device_timers_next_run_at"), ["next_run_at"]),
            (batch_op.f("ix_device_timers_user_id"), ["user_id"]),
        ):
            if idx_name not in existing_idx:
                batch_op.create_index(idx_name, cols, unique=False)


def downgrade() -> None:
    insp = sa.inspect(op.get_bind())
    if not insp.has_table("device_timers"):
        return
    existing_idx = {idx["name"] for idx in insp.get_indexes("device_timers")}
    with op.batch_alter_table("device_timers", schema=None) as batch_op:
        for idx_name in (
            batch_op.f("ix_device_timers_user_id"),
            batch_op.f("ix_device_timers_next_run_at"),
            batch_op.f("ix_device_timers_device_uuid"),
        ):
            if idx_name in existing_idx:
                batch_op.drop_index(idx_name)
    op.drop_table("device_timers")
