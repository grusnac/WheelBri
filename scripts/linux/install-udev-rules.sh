#!/usr/bin/env bash
# Once per machine (Steam Deck: set a password first with `passwd`, sudo needs it):
#  - lets the logged-in user create the virtual G920 (/dev/uhid) without root;
#  - lets games open the virtual G920 raw (its /dev/hidraw node), which Proton needs for its force feedback;
#  - lets wheelbri-service read the rig's wheel and accessories and play force feedback (their /dev/input/event nodes);
#  - loads the uhid module at every boot;
#  - lets that user run the service's input and force feedback threads above normal priority (nice -10, as Windows'
#    "above normal"), so a game loading every core does not hold the wheel's input back; from the next login.
# Undo: install-udev-rules.sh --remove

set -eu

rules=/etc/udev/rules.d/70-wheelbri.rules
modules=/etc/modules-load.d/wheelbri.conf
limits=/etc/security/limits.d/70-wheelbri.conf
user=${SUDO_USER:-$USER}   # run with or without sudo in front: the user at the desk either way
# The same files under the project's old name (wheelemu, before the rename): removed in both cases.
old_rules=/etc/udev/rules.d/70-wheelemu.rules
old_modules=/etc/modules-load.d/wheelemu.conf
if [ -e "$old_rules" ] || [ -e "$old_modules" ]; then
    sudo rm -f "$old_rules" "$old_modules"
    echo "removed $old_rules and $old_modules (the old name)"
fi

if [ "${1:-}" = "--remove" ]; then
    sudo rm -f "$rules" "$modules" "$limits"
    sudo udevadm control --reload-rules
    echo "removed $rules, $modules and $limits"
    exit 0
fi

sudo tee "$rules" >/dev/null <<'RULES'
# wheel-emulator (scripts/linux/install-udev-rules.sh)
# wheelbri-uhid creates the virtual G920, G25, G27 or G923 through uhid: allow the user at the seat.
KERNEL=="uhid", MODE="0660", TAG+="uaccess"
# Games (Proton with PROTON_ENABLE_HIDRAW=0x046D/0xC262) open the G920 raw for its force feedback; the virtual G25
# (0x046D/0xC299), G27 (0x046D/0xC29B) and G923 (0x046D/0xC26E) are on the virtual bus (0006), where the kernel's
# generic driver takes them.
SUBSYSTEM=="hidraw", KERNELS=="0003:046D:C262.*", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="hidraw", KERNELS=="0006:046D:C299.*", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="hidraw", KERNELS=="0006:046D:C29B.*", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="hidraw", KERNELS=="0006:046D:C26E.*", MODE="0660", TAG+="uaccess"
# wheelbri-service reads the rig's devices (any game controller) and writes force feedback through evdev.
SUBSYSTEM=="input", KERNEL=="event*", ENV{ID_INPUT_JOYSTICK}=="1", MODE="0660", TAG+="uaccess"
RULES
echo uhid | sudo tee "$modules" >/dev/null
printf '# wheel-emulator (scripts/linux/install-udev-rules.sh): wheelbri-service may raise its threads to nice -10\n%s - nice -10\n' \
    "$user" | sudo tee "$limits" >/dev/null

sudo modprobe uhid
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc --sysname-match=uhid
sudo udevadm trigger --subsystem-match=hidraw
sudo udevadm trigger --subsystem-match=input

echo "installed $rules, $modules and $limits (for $user: log out and in once, or restart the Deck)"
ls -l /dev/uhid
if command -v getfacl >/dev/null; then getfacl -p /dev/uhid 2>/dev/null | grep "user:" || true; fi
