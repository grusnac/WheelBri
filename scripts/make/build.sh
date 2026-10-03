#!/usr/bin/env bash
# Build what changed: core, service, shim and tests (configures the build tree the first time).
#
#   scripts/make/build.sh                       everything
#   scripts/make/build.sh wheelbri-service      one target (also: dinput8, wheelbri_tests, wheelbri_core)
#   WE_BUILD_TYPE=Debug scripts/make/build.sh   debug build

source "$(dirname "$0")/common.sh"

cmake_build "$@"
ok "built: $(service_exe)"
