"""Tools for capturing and decoding a real Logitech G923 (Xbox One and PC, 046D:C26E).

Standard library only, so it runs on MSYS2's or Windows' own Python without packages. See README.md.
"""

VENDOR_ID = 0x046D
PRODUCT_ID = 0xC26E          # G923 for Xbox One and PC, PC mode
OTHER_G923_IDS = {
    0xC26D: "G923 for Xbox One and PC, Xbox mode (as recorded by others; check)",
    0xC266: "G923 for PlayStation and PC (PS mode)",
    0xC267: "G923 for PlayStation and PC (PC mode)",
}


def rehearsal_target() -> tuple[int, int | None] | None:
    """G923_TARGET=VID[:PID] (hex, e.g. 346E for the MOZA R3): a rehearsal with another wheel. None = the G923."""
    import os
    text = os.environ.get("G923_TARGET", "").strip()
    if not text:
        return None
    vid, _, pid = text.partition(":")
    return int(vid, 16), (int(pid, 16) if pid and pid.lower() != "any" else None)


def is_target(vendor: int, product: int) -> bool:
    r = rehearsal_target()
    if r is None:
        return vendor == VENDOR_ID and (product == PRODUCT_ID or product in OTHER_G923_IDS)
    return vendor == r[0] and (r[1] is None or product == r[1])
