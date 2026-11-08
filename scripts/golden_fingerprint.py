#!/usr/bin/env python3
# =============================================================================
# scripts/golden_fingerprint.py
# CCU-FOTA 摘要 golden 向量参考实现 (CCU-FOTA-DSN-CR-006 §10.2 / CR-014 D4)
# -----------------------------------------------------------------------------
# 与 src/fingerprint/canonical_encoder.cpp 完全同构的独立实现，用于生成
# tests/test_inventory_fingerprint.cpp 与 tests/ota/inventory_provider_test.cpp
# 的 golden 向量（C++ 测试与 Python 参考实现互校）。
#
# 编码格式（大端）：
#   record := domain || field_count:u32be || field*
#   domain := length:u32be || utf8_bytes
#   field  := field_id:u16be || state:u8 || length:u32be || bytes
#   state  := 0=missing, 1=null, 2=present
#
# 用法: python3 scripts/golden_fingerprint.py
# =============================================================================
import hashlib
import struct

MISSING, NULL, PRESENT = 0, 1, 2

BASELINE_FACTORY, BASELINE_LAST_OTA, BASELINE_UNKNOWN = 1, 2, 3
VERSION_UDS_0x22, VERSION_SOMEIP_GET_VERSION = 1, 2
ECU_OK, ECU_MISSING, ECU_NRC, ECU_TIMEOUT, ECU_UNREACHABLE, ECU_PARSE_ERROR = 1, 2, 3, 4, 5, 6
RESULT_ALL_OK, RESULT_PARTIAL, RESULT_FAILED = 1, 2, 3
VIN_PROVISIONED, VIN_UNKNOWN = 1, 2

VERSION_DOMAIN = "cgw-fota-version-v2"
SNAPSHOT_DOMAIN = "cgw-fota-snapshot-v2"
DEDUPE_DOMAIN = "cgw-fota-dedupe-v1"
ID_DOMAIN_PREFIX = "cgw-fota-id-v1/"
WIRE_DOMAIN = "inventory-ecu-list-v2"
DESTINATION = "tbox-software-inventory"


class Encoder:
    def __init__(self, domain=""):
        self.buf = bytearray()
        self._u32(len(domain))
        self.buf += domain.encode()
        self.count_pos = len(self.buf)
        self._u32(0)
        self.count = 0

    def _u8(self, v):
        self.buf += struct.pack(">B", v)

    def _u16(self, v):
        self.buf += struct.pack(">H", v)

    def _u32(self, v):
        self.buf += struct.pack(">I", v)

    def _header(self, fid, state, length):
        self._u16(fid)
        self._u8(state)
        self._u32(length)

    def string(self, fid, value):
        raw = value.encode()
        self._header(fid, PRESENT, len(raw))
        self.buf += raw
        self.count += 1

    def optional_string(self, fid, value):
        if value is None:
            self._header(fid, MISSING, 0)
        else:
            raw = value.encode()
            self._header(fid, PRESENT, len(raw))
            self.buf += raw
        self.count += 1

    def enum(self, fid, code):
        self._header(fid, PRESENT, 4)
        self._u32(code)
        self.count += 1

    def bytes_field(self, fid, data):
        self._header(fid, PRESENT, len(data))
        self.buf += data
        self.count += 1

    def list_field(self, fid, items):
        total = 4 + sum(len(i) for i in items)
        self._header(fid, PRESENT, total)
        self._u32(len(items))
        for i in items:
            self.buf += i
        self.count += 1

    def finalize(self):
        self.buf[self.count_pos:self.count_pos + 4] = struct.pack(">I", self.count)
        return bytes(self.buf)


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------
# golden 输入（与 tests/test_inventory_fingerprint.cpp 的 makeGoldenSnapshot 一致）
# ---------------------------------------------------------------------------
ECUS = [
    # ecuId, partNumber(总成), softwarePartNumber(软件), swVersion, hwVersion, source, status, errorCode
    ("ECU1", "P001", "SP001", "1.2.3", "HW1", VERSION_UDS_0x22, ECU_OK, None),
    ("ECU2", "P002", "SP002", "2.0", None, VERSION_SOMEIP_GET_VERSION, ECU_OK, None),
]
VIN = "LSJAAAAAAAAAAAAAA"
BASELINE_ID = "bl-001"
REGISTRY_VERSION = "1.0.0"
COLLECTED_AT = "2026-08-07T10:00:00Z"


def version_ecu_sub(ecu):
    ecu_id, part, sw_part, sw, hw = ecu[0], ecu[1], ecu[2], ecu[3], ecu[4]
    sub = Encoder("")
    sub.string(0x0001, ecu_id)
    sub.optional_string(0x0002, part)
    sub.optional_string(0x0003, sw_part)
    sub.optional_string(0x0004, sw)
    sub.optional_string(0x0005, hw)
    return sub.finalize()


def snapshot_ecu_sub(ecu):
    ecu_id, part, sw_part, sw, hw, source, status, error = ecu
    sub = Encoder("")
    sub.string(0x0001, ecu_id)
    sub.optional_string(0x0002, part)
    sub.optional_string(0x0003, sw_part)
    sub.optional_string(0x0004, sw)
    sub.optional_string(0x0005, hw)
    sub.enum(0x0006, source)
    sub.enum(0x0007, status)
    sub.optional_string(0x0008, error)
    return sub.finalize()


def identifier_digest(kind, normalized_value):
    enc = Encoder(ID_DOMAIN_PREFIX + kind)
    enc.string(0x0001, normalized_value)
    return sha256_hex(enc.finalize())


def version_fingerprint_bytes():
    enc = Encoder(VERSION_DOMAIN)
    enc.enum(0x0001, BASELINE_FACTORY)
    enc.optional_string(0x0002, BASELINE_ID)
    enc.string(0x0003, REGISTRY_VERSION)
    enc.list_field(0x0004, [version_ecu_sub(e) for e in ECUS])
    return enc.finalize()


def snapshot_fingerprint_bytes():
    enc = Encoder(SNAPSHOT_DOMAIN)
    enc.string(0x0001, identifier_digest("vin", VIN))
    enc.enum(0x0002, VIN_PROVISIONED)
    enc.enum(0x0003, BASELINE_FACTORY)
    enc.optional_string(0x0004, BASELINE_ID)
    enc.string(0x0005, REGISTRY_VERSION)
    enc.enum(0x0006, RESULT_ALL_OK)
    enc.list_field(0x0007, [snapshot_ecu_sub(e) for e in ECUS])
    return enc.finalize()


def dedupe_key_bytes(snapshot_digest: bytes):
    enc = Encoder(DEDUPE_DOMAIN)
    enc.string(0x0001, DESTINATION)
    enc.string(0x0002, SNAPSHOT_DOMAIN)
    enc.bytes_field(0x0003, snapshot_digest)
    return enc.finalize()


def wire_digest_bytes():
    enc = Encoder(WIRE_DOMAIN)
    enc.list_field(0x0001, [version_ecu_sub(e) for e in ECUS])
    return enc.finalize()


def main():
    version_bytes = version_fingerprint_bytes()
    snapshot_bytes = snapshot_fingerprint_bytes()
    snapshot_digest = hashlib.sha256(snapshot_bytes).digest()
    dedupe_bytes = dedupe_key_bytes(snapshot_digest)
    wire_bytes = wire_digest_bytes()

    print(f"VERSION_DOMAIN   = {VERSION_DOMAIN}")
    print(f"versionFingerprint bytes = {version_bytes.hex()}")
    print(f"versionFingerprint       = {sha256_hex(version_bytes)}")
    print()
    print(f"SNAPSHOT_DOMAIN  = {SNAPSHOT_DOMAIN}")
    print(f"snapshotFingerprint bytes = {snapshot_bytes.hex()}")
    print(f"snapshotFingerprint       = {sha256_hex(snapshot_bytes)}")
    print()
    print(f"dedupeKey                 = {sha256_hex(dedupe_bytes)}")
    print()
    print(f"WIRE_DOMAIN      = {WIRE_DOMAIN}")
    print(f"wire ecuListDigest bytes = {wire_bytes.hex()}")
    print(f"wire ecuListDigest       = {sha256_hex(wire_bytes)}")
    print()
    print(f"identifierDigest(vin, {VIN})   = {identifier_digest('vin', VIN)}")
    print(f"identifierDigest(device_sn, SN12345) = {identifier_digest('device_sn', 'SN12345')}")
    print(f"collectedAt(not part of digests) = {COLLECTED_AT}")


if __name__ == "__main__":
    main()
