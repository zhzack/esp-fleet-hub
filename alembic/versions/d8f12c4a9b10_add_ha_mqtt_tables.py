"""add_ha_mqtt_tables

Revision ID: d8f12c4a9b10
Revises: c7a8e19b2d30
Create Date: 2026-11-27 01:18:00.000000
"""
from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa


revision: str = "d8f12c4a9b10"
down_revision: Union[str, Sequence[str], None] = "c7a8e19b2d30"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    insp = sa.inspect(op.get_bind())
    if not insp.has_table("ha_broker_configs"):
        op.create_table(
            "ha_broker_configs",
            sa.Column("id", sa.Integer(), nullable=False),
            sa.Column("enabled", sa.Boolean(), server_default="0", nullable=False),
            sa.Column("broker_host", sa.String(length=255), server_default="", nullable=False),
            sa.Column("broker_port", sa.Integer(), server_default="1883", nullable=False),
            sa.Column("mqtt_user", sa.String(length=128), server_default="", nullable=False),
            sa.Column("mqtt_pass", sa.Text(), server_default="", nullable=False),
            sa.Column("discovery_prefix", sa.String(length=64), server_default="homeassistant", nullable=False),
            sa.Column("topic_prefix", sa.String(length=64), server_default="esp_hub", nullable=False),
            sa.Column("sync_interval_sec", sa.Integer(), server_default="10", nullable=False),
            sa.Column("updated_at", sa.Integer(), server_default="0", nullable=False),
            sa.PrimaryKeyConstraint("id"),
        )
    if not insp.has_table("ha_device_targets"):
        op.create_table(
            "ha_device_targets",
            sa.Column("id", sa.BigInteger().with_variant(sa.Integer, "sqlite"), autoincrement=True, nullable=False),
            sa.Column("device_uuid", sa.String(length=128), nullable=False),
            sa.Column("enabled", sa.Boolean(), server_default="1", nullable=False),
            sa.Column("display_name", sa.String(length=128), server_default="", nullable=False),
            sa.Column("area_name", sa.String(length=64), server_default="", nullable=False),
            sa.Column("expose_hid", sa.Boolean(), server_default="1", nullable=False),
            sa.Column("expose_gpio", sa.Boolean(), server_default="1", nullable=False),
            sa.Column("expose_display", sa.Boolean(), server_default="1", nullable=False),
            sa.Column("expose_camera", sa.Boolean(), server_default="1", nullable=False),
            sa.Column("pc_enabled", sa.Boolean(), server_default="0", nullable=False),
            sa.Column("pc_name", sa.String(length=128), server_default="", nullable=False),
            sa.Column("pc_ip", sa.String(length=64), server_default="", nullable=False),
            sa.Column("pc_mac", sa.String(length=32), server_default="", nullable=False),
            sa.Column("pc_broadcast_ip", sa.String(length=64), server_default="255.255.255.255", nullable=False),
            sa.Column("pc_power_pin", sa.Integer(), nullable=True),
            sa.Column("pc_power_pulse_ms", sa.Integer(), server_default="500", nullable=False),
            sa.Column("pc_os", sa.String(length=16), server_default="windows", nullable=False),
            sa.Column("legacy_topic_prefix", sa.String(length=64), server_default="", nullable=False),
            sa.Column("exposed_pins", sa.JSON(), nullable=True),
            sa.Column("updated_at", sa.Integer(), server_default="0", nullable=False),
            sa.PrimaryKeyConstraint("id"),
            sa.UniqueConstraint("device_uuid"),
        )
    idx_names = {idx["name"] for idx in sa.inspect(op.get_bind()).get_indexes("ha_device_targets")}
    if "ix_ha_device_targets_device_uuid" not in idx_names:
        with op.batch_alter_table("ha_device_targets", schema=None) as batch_op:
            batch_op.create_index("ix_ha_device_targets_device_uuid", ["device_uuid"], unique=True)


def downgrade() -> None:
    insp = sa.inspect(op.get_bind())
    if insp.has_table("ha_device_targets"):
        op.drop_table("ha_device_targets")
    if insp.has_table("ha_broker_configs"):
        op.drop_table("ha_broker_configs")
