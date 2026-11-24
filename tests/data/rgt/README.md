# Thrustmaster RGT FFB Clutch (044F:B653): public captures (test data)

Source: Didier Moens, linux-input, 2008-09-13, attachments of
https://lore.kernel.org/all/48CBD7B3.4070104@xs4all.be/ (2-dmesg.out, 3-messages.out, 4-evtest.out),
kernel 2.6.26 with hid debug=2. Real wheel.

- `b653-report-descriptor.hex`: the 455-byte report descriptor as the kernel printed it.
- Feature reports read at plug-in: 0x81 = 01 21 ff ff; 0x82 = 50 5a; 0x88 = 00 02; 0x86 = 00 01
  (0x85, 0x87: no data shown).
- Enumerated as low speed (ohci/ehci "new low speed USB device").
- evtest: X -512..511 fuzz 3 flat 63; Y, Rz, Throttle (Slider), Rudder (Dial) 0..255 flat 15; Hat0X/Y;
  buttons BTN_TRIGGER..BTN_BASE4 (10); EV_FF.
