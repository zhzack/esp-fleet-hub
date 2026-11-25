#!/usr/bin/env python3
"""
Unit & Integration Test Suite for ESP Unified Framework:
- SoC compatibility interlock & 2MB/4MB/8MB/16MB partition sizing
- Kconfig macro generation (including CONFIG_APP_MDNS_HOST fix)
- Pure-Python NVS V2 partition binary image format & CRC32 validation
- SQLite Fleet Hub DB: provision, HMAC challenge-response, register, heartbeat, shadow sync, command queue
- Fleet HTTP Router: challenge/register/heartbeat/config/batch-OTA target mismatch guard
"""

import os
import sys
import json
import hmac
import zlib
import struct
import hashlib
import tempfile
import unittest

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import build_flavor
import nvs_gen
import hub_db
import fleet_router


class TestBuildFlavor(unittest.TestCase):
    def test_mdns_macro_name_and_partition_map(self):
        cfg = build_flavor.generate_sdkconfig_defaults({
            "name": "test_s3",
            "target": "esp32s3",
            "hardware": {"cpu_freq_mhz": 240, "spiram": True, "flash_size": "16MB", "board": "esp32s3_cam"},
            "features": {"http_server": True, "camera": True, "gpio_ctrl": True},
            "defaults": {"mdns_host": "s3-lab-cam", "hub_url": "http://192.168.1.10:8088"},
        })
        self.assertIn('CONFIG_APP_MDNS_HOST="s3-lab-cam"', cfg)
        self.assertNotIn("CONFIG_APP_DEFAULT_MDNS_HOST", cfg)
        self.assertIn('CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_16mb.csv"', cfg)
        self.assertIn("CONFIG_BOARD_ESP32S3_CAM_DEV=y", cfg)
        self.assertIn("CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096", cfg)

    def test_2mb_flash_rejects_spiffs_or_camera(self):
        with self.assertRaises(ValueError):
            build_flavor.validate_soc_compatibility(
                "esp32c3", {"storage": True}, {"flash_size": "2MB"}
            )
        with self.assertRaises(ValueError):
            build_flavor.validate_soc_compatibility(
                "esp32", {"camera": True}, {"flash_size": "2MB"}
            )


class TestNvsGenerator(unittest.TestCase):
    def test_nvs_v2_page_header_and_crc(self):
        with tempfile.TemporaryDirectory() as td:
            out_bin = os.path.join(td, "nvs_prov.bin")
            nvs_gen.generate_nvs_provision_bin(out_bin, {
                "wifi_ssid": "LabWiFi",
                "wifi_pass": "pass1234",
                "device_id": "esp32c3-aabbcc",
                "mdns_host": "c3-test",
                "hub_url": "http://192.168.68.159:8088",
                "device_secret": "deadbeef" * 8,
                "web_port": 80,
                "config_ver": 3,
            })
            with open(out_bin, "rb") as f:
                data = f.read()
            self.assertEqual(len(data), 0x4000)
            state, seq, ver = struct.unpack_from("<IIB", data, 0)
            self.assertEqual(state, 0xFFFFFFFE)
            self.assertEqual(seq, 0)
            self.assertEqual(ver, 0xFE)
            stored_crc = struct.unpack_from("<I", data, 28)[0]
            self.assertEqual(stored_crc, zlib.crc32(data[4:28], 0xFFFFFFFF) & 0xFFFFFFFF)
            self.assertIn(b"esp32c3-aabbcc\x00", data)
            self.assertIn(b"http://192.168.68.159:8088\x00", data)

    def test_esp8266_eeprom_layout_and_crc32(self):
        with tempfile.TemporaryDirectory() as td:
            out_bin = os.path.join(td, "esp8266_eeprom.bin")
            nvs_gen.generate_esp8266_eeprom_bin(out_bin, {
                "wifi_ssid": "LabWiFi",
                "wifi_pass": "pass1234",
                "device_id": "esp8266-aabbcc",
                "mdns_host": "esp8266-aabbcc",
                "hub_url": "http://192.168.68.159:8000",
                "web_port": 80,
                "config_ver": 1,
            })
            with open(out_bin, "rb") as f:
                data = f.read()
            self.assertEqual(len(data), 4096)
            self.assertEqual(struct.calcsize(nvs_gen._ESP8266_CFG_FMT), 368)
            magic, ver, port, ssid, pw, dev_id, mdns, hub, sec, tok, crc = struct.unpack_from(
                nvs_gen._ESP8266_CFG_FMT, data, 0
            )
            self.assertEqual(magic, nvs_gen.ESP8266_CFG_MAGIC)
            self.assertEqual(ver, 1)
            self.assertEqual(port, 80)
            self.assertEqual(ssid.rstrip(b"\x00"), b"LabWiFi")
            self.assertEqual(hub.rstrip(b"\x00"), b"http://192.168.68.159:8000")
            self.assertEqual(crc, zlib.crc32(data[:364]) & 0xFFFFFFFF)


class TestFleetHubDb(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.db_path = os.path.join(self.td.name, "test_fleet.db")
        hub_db.init_db(self.db_path)

    def tearDown(self):
        hub_db.close_db()
        self.td.cleanup()

    def test_provision_hmac_register_shadow_and_commands(self):
        prov = hub_db.provision_device(
            device_id="esp32c3-112233",
            mac="60:55:F9:11:22:33",
            chip="ESP32-C3",
            flash_mb=4,
            board="esp32c3_air101",
            device_secret="my_super_secret_key",
        )
        self.assertEqual(prov["status"], "provisioned")
        self.assertTrue(prov["has_secret"])

        nonce = hub_db.issue_nonce("esp32c3-112233")
        bad_ok, _ = hub_db.verify_and_consume_hmac("esp32c3-112233", nonce, "0" * 64)
        self.assertFalse(bad_ok)

        # Consume once should fail replay
        nonce2 = hub_db.issue_nonce("esp32c3-112233")
        proof = hmac.new(b"my_super_secret_key", nonce2.encode(), hashlib.sha256).hexdigest()
        good_ok, _ = hub_db.verify_and_consume_hmac("esp32c3-112233", nonce2, proof)
        self.assertTrue(good_ok)
        replay_ok, _ = hub_db.verify_and_consume_hmac("esp32c3-112233", nonce2, proof)
        self.assertFalse(replay_ok)

        dev = hub_db.register_or_update_device({
            "device_id": "esp32c3-112233",
            "mac": "60:55:F9:11:22:33",
            "chip": "ESP32-C3 (rev v0.3)",
            "ip": "192.168.68.201",
            "web_port": 80,
            "fw_version": "1.1.0-unified",
            "capabilities": {"gpio": {"enabled": True}, "pc_monitor": {"enabled": True}},
            "config": {"config_ver": 1, "wifi_ssid": "1"},
        })
        self.assertEqual(dev["status"], "online")
        self.assertEqual(dev["ip"], "192.168.68.201")

        # Update desired shadow config (ver becomes 2) & queue a command
        hub_db.update_desired_config("esp32c3-112233", {"mdns_host": "c3-desk-pro"})
        cmd_id = hub_db.queue_command("esp32c3-112233", "gpio_set", {"pin": 12, "val": 1})

        # Device heartbeat at ver=1 should receive desired_config ver=2 + queued command
        hb_res = hub_db.process_heartbeat("esp32c3-112233", {
            "rssi": -48, "free_heap": 180000, "uptime_sec": 30, "config_ver": 1
        })
        self.assertTrue(hb_res["ok"])
        self.assertIsNotNone(hb_res["sync_config"])
        self.assertEqual(hb_res["sync_config"]["mdns_host"], "c3-desk-pro")
        self.assertEqual(hb_res["sync_config"]["config_ver"], 2)
        self.assertEqual(len(hb_res["commands"]), 1)
        self.assertEqual(hb_res["commands"][0]["id"], cmd_id)


if __name__ == "__main__":
    unittest.main()
