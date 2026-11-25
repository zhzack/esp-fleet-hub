#!/usr/bin/env python3
"""
Pure-Python ESP-IDF NVS V2 Partition Image Generator
Generates an exact 0x4000-byte (16KB) binary NVS image pre-populated with
namespace 'app' entries (device_id, wifi_ssid, wifi_pass, hub_url, dev_secret, etc.)
so every board gets unique factory credentials flashed directly into 0x9000!
"""

import struct
import zlib
import os

NVS_PAGE_SIZE = 4096
NVS_ENTRY_SIZE = 32
NVS_ENTRIES_PER_PAGE = 126
NVS_HEADER_SIZE = 32
NVS_BITMAP_SIZE = 32

# NVS Entry Types (from nvs_types.hpp)
NVS_TYPE_U8   = 0x01
NVS_TYPE_U16  = 0x02
NVS_TYPE_U32  = 0x04
NVS_TYPE_STR  = 0x21

# Page states (written in inverted-bit order in flash)
PAGE_STATE_ACTIVE = 0xFFFFFFFE


def _crc32_esp(data: bytes) -> int:
    # ESP-IDF NVS uses esp_rom_crc32_le(UINT32_MAX, data, len), which equals
    # Python zlib.crc32(data, 0xFFFFFFFF) & 0xFFFFFFFF.
    return zlib.crc32(data, 0xFFFFFFFF) & 0xFFFFFFFF


class NvsBuilder:
    def __init__(self, partition_size=0x4000):
        self.partition_size = partition_size
        self.num_pages = partition_size // NVS_PAGE_SIZE
        self.pages = [bytearray(b"\xFF" * NVS_PAGE_SIZE) for _ in range(self.num_pages)]
        self.page_idx = 0
        self.entry_idx = 0
        self.namespaces = {}
        self.next_ns_id = 1
        self._init_page(0, seq=0)

    def _init_page(self, p_idx, seq):
        buf = self.pages[p_idx]
        # Page header (32 bytes):
        # 0..3: State (ACTIVE = 0xFFFFFFFE)
        # 4..7: SeqNo (uint32)
        # 8: Version (0xFE = V2)
        # 9..27: Unused (0xFF)
        # 28..31: CRC32 over bytes 4..27 (24 bytes)
        struct.pack_into("<IIB", buf, 0, PAGE_STATE_ACTIVE, seq, 0xFE)
        crc = _crc32_esp(bytes(buf[4:28]))
        struct.pack_into("<I", buf, 28, crc)

    def _set_entry_written(self, p_idx, e_idx):
        # 2 bits per entry in 32-byte bitmap (starts at offset 32):
        # 11 = empty, 10 = written (binary bit pattern 0b10), 00 = erased
        byte_offset = NVS_HEADER_SIZE + (e_idx // 4)
        shift = (e_idx % 4) * 2
        # Clear lower bit of the 2-bit pair to turn 0b11 -> 0b10 (Written)
        self.pages[p_idx][byte_offset] &= ~(1 << shift)

    def _alloc_entries(self, count):
        if self.entry_idx + count > NVS_ENTRIES_PER_PAGE:
            self.page_idx += 1
            if self.page_idx >= self.num_pages:
                raise MemoryError("NVS partition full")
            self._init_page(self.page_idx, seq=self.page_idx)
            self.entry_idx = 0
        start = self.entry_idx
        for i in range(count):
            self._set_entry_written(self.page_idx, start + i)
        self.entry_idx += count
        return self.page_idx, start

    def _write_raw_entry(self, p_idx, e_idx, ns_idx, data_type, span, chunk_idx, key_str, data8_bytes):
        offset = NVS_HEADER_SIZE + NVS_BITMAP_SIZE + (e_idx * NVS_ENTRY_SIZE)
        buf = self.pages[p_idx]

        key_bytes = key_str.encode("utf-8")[:15]
        key_padded = key_bytes.ljust(16, b"\x00")

        # Entry layout (32 bytes):
        # 0: Ns (uint8)
        # 1: Type (uint8)
        # 2: Span (uint8)
        # 3: ChunkIndex (uint8)
        # 4..7: CRC32 (calculated over bytes 0..3 + bytes 8..31)
        # 8..23: Key (16 bytes)
        # 24..31: Data (8 bytes)
        hdr4 = struct.pack("<BBBB", ns_idx, data_type, span, chunk_idx)
        payload24 = key_padded + data8_bytes
        crc = _crc32_esp(hdr4 + payload24)
        struct.pack_into("<I", buf, offset + 4, crc)
        buf[offset : offset + 4] = hdr4
        buf[offset + 8 : offset + 32] = payload24

    def _ensure_namespace(self, ns_name):
        if ns_name in self.namespaces:
            return self.namespaces[ns_name]
        ns_id = self.next_ns_id
        self.next_ns_id += 1
        self.namespaces[ns_name] = ns_id
        p_idx, e_idx = self._alloc_entries(1)
        data8 = struct.pack("<B", ns_id) + b"\xFF" * 7
        self._write_raw_entry(p_idx, e_idx, 0, NVS_TYPE_U8, 1, 0xFF, ns_name, data8)
        return ns_id

    def set_u16(self, ns_name, key, val):
        ns_id = self._ensure_namespace(ns_name)
        p_idx, e_idx = self._alloc_entries(1)
        data8 = struct.pack("<H", val & 0xFFFF) + b"\xFF" * 6
        self._write_raw_entry(p_idx, e_idx, ns_id, NVS_TYPE_U16, 1, 0xFF, key, data8)

    def set_u32(self, ns_name, key, val):
        ns_id = self._ensure_namespace(ns_name)
        p_idx, e_idx = self._alloc_entries(1)
        data8 = struct.pack("<I", val & 0xFFFFFFFF) + b"\xFF" * 4
        self._write_raw_entry(p_idx, e_idx, ns_id, NVS_TYPE_U32, 1, 0xFF, key, data8)

    def set_str(self, ns_name, key, val_str):
        ns_id = self._ensure_namespace(ns_name)
        raw = val_str.encode("utf-8") + b"\x00"
        data_entries = (len(raw) + NVS_ENTRY_SIZE - 1) // NVS_ENTRY_SIZE
        span = 1 + data_entries
        p_idx, e_idx = self._alloc_entries(span)

        # Padded data rounded to multiple of 32
        padded_len = data_entries * NVS_ENTRY_SIZE
        padded_data = raw + b"\xFF" * (padded_len - len(raw))
        data_crc = _crc32_esp(raw)
        data8 = struct.pack("<HHI", len(raw), 0xFFFF, data_crc)
        self._write_raw_entry(p_idx, e_idx, ns_id, NVS_TYPE_STR, span, 0xFF, key, data8)

        data_off = NVS_HEADER_SIZE + NVS_BITMAP_SIZE + ((e_idx + 1) * NVS_ENTRY_SIZE)
        self.pages[p_idx][data_off : data_off + padded_len] = padded_data

    def to_bytes(self):
        return b"".join(self.pages)


def generate_nvs_provision_bin(out_path, config_dict, partition_size=0x4000):
    """Build an NVS partition binary containing namespace 'app' provisioning entries."""
    builder = NvsBuilder(partition_size=partition_size)
    str_keys = [
        ("wifi_ssid", config_dict.get("wifi_ssid")),
        ("wifi_pass", config_dict.get("wifi_pass")),
        ("device_id", config_dict.get("device_id")),
        ("mdns_host", config_dict.get("mdns_host")),
        ("hub_url", config_dict.get("hub_url")),
        ("gw_ws_url", config_dict.get("ws_gateway_url")),
        ("dev_secret", config_dict.get("device_secret")),
        ("api_token", config_dict.get("api_token")),
    ]
    for k, v in str_keys:
        if v is not None and str(v) != "":
            builder.set_str("app", k, str(v))

    if config_dict.get("web_port"):
        builder.set_u16("app", "web_port", int(config_dict["web_port"]))
    builder.set_u32("app", "cfg_ver", int(config_dict.get("config_ver", 1)))

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(builder.to_bytes())
    return out_path


ESP8266_CFG_MAGIC = 0x45554633
ESP8266_EEPROM_OFFSET_1M = 0xFB000
_ESP8266_CFG_FMT = "<IIH33s65s32s32s96s48s48sI"


def _c_str_bytes(val: str | None, max_len: int) -> bytes:
    raw = str(val or "").encode("utf-8")[: max(0, max_len - 1)]
    return raw.ljust(max_len, b"\x00")


def generate_esp8266_eeprom_bin(out_path: str, config_dict: dict) -> str:
    """Build a 4KB sector-aligned ESP8266 EEPROM provisioning block matching struct AppCfg."""
    cfg_ver = int(config_dict.get("config_ver", 1) or 1)
    web_port = int(config_dict.get("web_port", 80) or 80)
    body = struct.pack(
        "<IIH33s65s32s32s96s48s48s",
        ESP8266_CFG_MAGIC,
        cfg_ver,
        web_port,
        _c_str_bytes(config_dict.get("wifi_ssid"), 33),
        _c_str_bytes(config_dict.get("wifi_pass"), 65),
        _c_str_bytes(config_dict.get("device_id"), 32),
        _c_str_bytes(config_dict.get("mdns_host"), 32),
        _c_str_bytes(config_dict.get("hub_url"), 96),
        _c_str_bytes(config_dict.get("device_secret"), 48),
        _c_str_bytes(config_dict.get("api_token"), 48),
    )
    crc = zlib.crc32(body) & 0xFFFFFFFF
    cfg_bytes = body + struct.pack("<I", crc)
    sector = bytearray(b"\xFF" * 4096)
    sector[0 : len(cfg_bytes)] = cfg_bytes

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(sector)
    return out_path
