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
