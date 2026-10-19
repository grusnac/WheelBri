#!/usr/bin/env bash
# Build the service and open its mapping window: the quick check after a mapping or config change.
# Close the window to stop. Extra arguments go to the service (e.g. --virtual-wheel, needs Administrator).
#
#   scripts/make/ui.sh                                    config/g920.ini
#   scripts/make/ui.sh config/g25.ini --print

source "$(dirname "$0")/common.sh"

config=config/g920.ini
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then
    config=$1
    shift
fi
[ -f "$config" ] || fail "no config at $config"

cmake_build wheelbri-service
say "starting $(service_exe) --config $config --ui $*"
"$(service_exe)" --config "$config" --ui "$@"
