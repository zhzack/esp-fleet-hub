#!/usr/bin/env python3
"""
Pure-Python Zero-Dependency mDNS Service Advertiser (_esp-hub._tcp.local)
Responds to ESP-IDF mdns_query_ptr("_esp-hub", "_tcp") multicast queries on 224.0.0.251:5353
so ESP32 / ESP8266 devices discover the Fleet Hub LAN IP & port with zero manual configuration.
"""

import socket
import struct
import threading
import time

MDNS_ADDR = "224.0.0.251"
MDNS_PORT = 5353

DNS_TYPE_A = 1
DNS_TYPE_PTR = 12
DNS_TYPE_TXT = 16
DNS_TYPE_SRV = 33


def get_lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def _encode_name(name: str) -> bytes:
    parts = [p.encode("utf-8") for p in name.strip(".").split(".") if p]
    return b"".join(struct.pack("B", len(p)) + p for p in parts) + b"\x00"


def _decode_name(pkt: bytes, offset: int):
    labels = []
    orig_off = offset
    jumped = False
    seen = set()
    while offset < len(pkt):
        if offset in seen:
            break
        seen.add(offset)
        length = pkt[offset]
        if length == 0:
            offset += 1
            break
        if (length & 0xC0) == 0xC0:
            if offset + 1 >= len(pkt):
                break
            ptr = ((length & 0x3F) << 8) | pkt[offset + 1]
            if not jumped:
                orig_off = offset + 2
            offset = ptr
            jumped = True
            continue
        offset += 1
        labels.append(pkt[offset : offset + length].decode("utf-8", "ignore"))
        offset += length
    return ".".join(labels), (orig_off if jumped else offset)


class MdnsHubAdvertiser:
    def __init__(self, port=8088, instance="ESP-Unified-Hub", hostname="esp-hub-host"):
        self.port = int(port)
        self.instance_fqdn = f"{instance}._esp-hub._tcp.local"
        self.service_fqdn = "_esp-hub._tcp.local"
        self.host_fqdn = f"{hostname}.local"
        self._stop = False
        self._thread = None
        self._sock = None

    def _build_response(self):
        lan_ip = get_lan_ip()
        ip_bytes = socket.inet_aton(lan_ip)
        ttl = 120

        # Header: ID=0, Flags=0x8400 (Response + Authoritative), QD=0, AN=1, NS=0, AR=3
        hdr = struct.pack(">HHHHHH", 0, 0x8400, 0, 1, 0, 3)

        # Answer 1: PTR _esp-hub._tcp.local -> ESP-Unified-Hub._esp-hub._tcp.local
        ptr_rdata = _encode_name(self.instance_fqdn)
        ans_ptr = (
            _encode_name(self.service_fqdn)
            + struct.pack(">HHIH", DNS_TYPE_PTR, 0x0001, ttl, len(ptr_rdata))
            + ptr_rdata
        )

        # Additional 1: SRV ESP-Unified-Hub._esp-hub._tcp.local
        srv_rdata = struct.pack(">HHH", 0, 0, self.port) + _encode_name(self.host_fqdn)
        add_srv = (
            _encode_name(self.instance_fqdn)
            + struct.pack(">HHIH", DNS_TYPE_SRV, 0x8001, ttl, len(srv_rdata))
            + srv_rdata
        )

        # Additional 2: TXT ESP-Unified-Hub._esp-hub._tcp.local
        txt_str = b"ver=1.1"
        txt_rdata = struct.pack("B", len(txt_str)) + txt_str
        add_txt = (
            _encode_name(self.instance_fqdn)
            + struct.pack(">HHIH", DNS_TYPE_TXT, 0x8001, ttl, len(txt_rdata))
            + txt_rdata
        )

        # Additional 3: A esp-hub-host.local -> LAN IPv4
        add_a = (
            _encode_name(self.host_fqdn)
            + struct.pack(">HHIH", DNS_TYPE_A, 0x8001, ttl, 4)
            + ip_bytes
        )

        return hdr + ans_ptr + add_srv + add_txt + add_a

    def _run(self):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):
                try:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
                except OSError:
                    pass
            sock.bind(("", MDNS_PORT))

            lan_ip = get_lan_ip()
            try:
                mreq = socket.inet_aton(MDNS_ADDR) + socket.inet_aton(lan_ip)
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(lan_ip))
            except OSError:
                mreq = socket.inet_aton(MDNS_ADDR) + socket.inet_aton("0.0.0.0")
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            sock.settimeout(1.5)
            self._sock = sock

            # Unsolicited startup announcement
            rsp = self._build_response()
            sock.sendto(rsp, (MDNS_ADDR, MDNS_PORT))
            last_announce = time.time()

            while not self._stop:
                if time.time() - last_announce >= 30.0:
                    try:
                        sock.sendto(self._build_response(), (MDNS_ADDR, MDNS_PORT))
                    except OSError:
                        pass
                    last_announce = time.time()

                try:
                    data, addr = sock.recvfrom(2048)
                except socket.timeout:
                    continue
                except OSError:
                    break

                if len(data) < 12:
                    continue
                flags, qdcount = struct.unpack(">HH", data[2:6])
                if (flags & 0x8000) != 0 or qdcount == 0:
                    continue

                offset = 12
                should_reply = False
                for _ in range(min(qdcount, 10)):
                    if offset >= len(data):
                        break
                    qname, offset = _decode_name(data, offset)
                    if offset + 4 > len(data):
                        break
                    qtype, _ = struct.unpack(">HH", data[offset : offset + 4])
                    offset += 4
                    ql = qname.lower()
                    if any(
                        t in ql
                        for t in ("_esp-hub._tcp", "_services._dns-sd", "esp-hub-host")
                    ):
                        should_reply = True
                        break

                if should_reply:
                    try:
                        sock.sendto(self._build_response(), (MDNS_ADDR, MDNS_PORT))
                    except OSError:
                        pass
        except Exception as e:
            print(f"[mDNS Advertiser] Disabled/Warn: {e}")

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop = True
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
