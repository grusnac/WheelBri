"""HID++ 2.0 reports: short 0x10 (7 bytes), long 0x11 (20), very long 0x12 (64):
[report ID, device index, feature index, function << 4 | software id, parameters...]. An error answer is
[report ID, device index, 0xFF, feature index, function | sw id, error code]; HID++ 1.0's is [.., 0x8F, ...]."""

from __future__ import annotations

FEATURES = {
    0x0000: "Root",
    0x0001: "FeatureSet",
    0x0002: "FeatureInfo",
    0x0003: "DeviceFwVersion",
    0x0005: "DeviceNameType",
    0x0007: "DeviceFriendlyName",
    0x0020: "ConfigChange",
    0x1E00: "EnableHiddenFeatures",
    0x1E22: "SPI direct access",
    0x1801: "Manufacturing mode",
    0x1802: "Device reset",
    0x1803: "GPIO access",
    0x1890: "RF test",
    0x18A1: "LED test",
    0x1F1F: "Firmware properties",
    0x8040: "Brightness control",
    0x8070: "Color LED effects",
    0x8071: "RGB effects",
    0x80A4: "Axis response curve",
    0x8123: "ForceFeedback",
    0x8127: "Peripheral tuning (?)",
    0x812C: "TrueForce (?)",
}

ERRORS = {0: "no error", 1: "unknown", 2: "invalid argument", 3: "out of range", 4: "hardware error",
          5: "Logitech internal", 6: "invalid feature index", 7: "invalid function", 8: "busy", 9: "unsupported"}

REPORT_IDS = (0x10, 0x11, 0x12)


def is_hidpp(data: bytes) -> bool:
    return len(data) >= 4 and data[0] in REPORT_IDS and len(data) in (7, 20, 64)


def is_error(answer: bytes) -> bool:
    return len(answer) >= 6 and answer[2] in (0xFF, 0x8F)


def answers(request: bytes, answer: bytes) -> bool:
    if len(answer) < 5 or answer[1] != request[1]:
        return False
    if is_error(answer):
        return answer[3] == request[2] and answer[4] == request[3]
    return answer[2] == request[2] and answer[3] == request[3]


def feature_name(feature_id: int | None) -> str:
    if feature_id is None:
        return "?"
    return f"0x{feature_id:04X} {FEATURES.get(feature_id, '')}".strip()


def ascii_of(data: bytes) -> str:
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data)
