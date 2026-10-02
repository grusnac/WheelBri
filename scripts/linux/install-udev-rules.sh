#!/usr/bin/env bash
# Once per machine (Steam Deck: set a password first with `passwd`, sudo needs it):
#  - lets the logged-in user create the virtual G920 (/dev/uhid) without root;
#  - lets games open the virtual G920 raw (its /dev/hidraw node), which Proton needs for its force feedback;
#  - lets wheelemu-service read the Moza wheel and play its force feedback (its /dev/input/event node);
#  - loads the uhid module at every boot.
# Undo: install-udev-rules.sh --remove

set -eu

rules=/etc/udev/rules.d/70-wheelemu.rules
modules=/etc/modules-load.d/wheelemu.conf

if [ "${1:-}" = "--remove" ]; then
    sudo rm -f "$rules" "$modules"
    sudo udevadm control --reload-rules
    echo "removed $rules and $modules"
    exit 0
fi

sudo tee "$rules" >/dev/null <<'RULES'
# wheel-emulator (scripts/linux/install-udev-rules.sh)
# wheelemu-uhid creates the virtual G920 through uhid: allow the user at the seat.
KERNEL=="uhid", MODE="0660", TAG+="uaccess"
# Games (Proton with PROTON_ENABLE_HIDRAW=0x046D/0xC262) open the G920 raw for its force feedback.
SUBSYSTEM=="hidraw", KERNELS=="0003:046D:C262.*", MODE="0660", TAG+="uaccess"
# wheelemu-service reads the Moza wheel (vendor 346e) and writes its force feedback through evdev.
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{idVendor}=="346e", MODE="0660", TAG+="uaccess"
RULES
echo uhid | sudo tee "$modules" >/dev/null

sudo modprobe uhid
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc --sysname-match=uhid
sudo udevadm trigger --subsystem-match=hidraw
sudo udevadm trigger --subsystem-match=input

echo "installed $rules and $modules"
ls -l /dev/uhid
if command -v getfacl >/dev/null; then getfacl -p /dev/uhid 2>/dev/null | grep "user:" || true; fi
