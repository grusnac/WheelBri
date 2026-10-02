#!/usr/bin/env bash
# Build the service and open its mapping window: the quick check after a mapping or config change.
# Close the window to stop. Extra arguments go to the service (e.g. --virtual-g920, needs Administrator).
#
#   scripts/make/ui.sh                                    config/moza-r3-as-g920.ini
#   scripts/make/ui.sh config/moza-r3-as-g29.ini          another config
#   scripts/make/ui.sh config/moza-r3-as-g920.ini --print

source "$(dirname "$0")/common.sh"

config=config/moza-r3-as-g920.ini
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then
    config=$1
    shift
fi
[ -f "$config" ] || fail "no config at $config"

cmake_build wheelemu-service
say "starting $(service_exe) --config $config --ui $*"
"$(service_exe)" --config "$config" --ui "$@"
