#!/usr/bin/env bash
# Once per machine (Steam Deck: set a password first with `passwd`, sudo needs it):
#  - lets that user create the virtual wheel (G25, G27, G920 or G923: /dev/uhid) without root, as a member of the
#    group wheelbri: /dev/uhid can make any device (a keyboard too), so only that group may, not every program of
#    whoever sits at the machine;
#  - lets games open the virtual wheel raw (its /dev/hidraw node), which Proton needs for its force feedback;
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
group=wheelbri             # may use /dev/uhid
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
    if getent group "$group" >/dev/null; then sudo groupdel "$group"; fi
    echo "removed $rules, $modules, $limits and the group $group"
    exit 0
fi

getent group "$group" >/dev/null || sudo groupadd --system "$group"
sudo usermod -aG "$group" "$user"

sudo tee "$rules" >/dev/null <<'RULES'
# WheelBri (scripts/linux/install-udev-rules.sh)
# wheelbri-uhid creates the virtual G25, G27, G920 or G923 through uhid: the group wheelbri only (uhid can make any
# device, a keyboard too: not every program of the user at the seat).
KERNEL=="uhid", GROUP="wheelbri", MODE="0660"
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
printf '# WheelBri (scripts/linux/install-udev-rules.sh): wheelbri-service may raise its threads to nice -10\n%s - nice -10\n' \
    "$user" | sudo tee "$limits" >/dev/null

sudo modprobe uhid
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc --sysname-match=uhid
sudo udevadm trigger --subsystem-match=hidraw
sudo udevadm trigger --subsystem-match=input

echo "installed $rules, $modules and $limits; $user is in the group $group (log out and in once, or restart the Deck)"
ls -l /dev/uhid
