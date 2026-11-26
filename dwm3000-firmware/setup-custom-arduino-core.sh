#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
FIRMWARE_DIR="$SCRIPT_DIR"
UWB_NODE_DIR="$FIRMWARE_DIR/UwbNode"
DRIVER_LIB="$UWB_NODE_DIR/lib/Makerfabs-ESP32-UWB-DW3000"
REPO_ROOT="$(git -C "$FIRMWARE_DIR" rev-parse --show-toplevel 2>/dev/null || true)"
CORE_CONFIG="$FIRMWARE_DIR/core-build.env"
LIBRARY_LIST="$FIRMWARE_DIR/arduino-libraries.txt"

log() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

[[ -f "$CORE_CONFIG" ]] || fail "Core build configuration is missing: $CORE_CONFIG"
[[ -f "$LIBRARY_LIST" ]] || fail "Arduino library list is missing: $LIBRARY_LIST"
# shellcheck disable=SC1090
source "$CORE_CONFIG"

COMPILE_FIRMWARE=1
usage() {
  cat <<USAGE
Usage: $0 [--core-only] [--help]

Build/install the custom eight-peer ESP32-C3 Arduino core. By default, install
the pinned Arduino libraries and compile UwbNode. Use --core-only to stop after
the custom core has been built and verified.
USAGE
}

while (($#)); do
  case "$1" in
    --core-only) COMPILE_FIRMWARE=0 ;;
    --help|-h) usage; exit 0 ;;
    *) fail "Unknown option: $1 (use --help)" ;;
  esac
  shift
done

ARDUINO_CLI="${ARDUINO_CLI:-arduino-cli}"
ARDUINO_CLI_PATH="$(command -v "$ARDUINO_CLI" 2>/dev/null || true)"
[[ -n "$ARDUINO_CLI_PATH" ]] || fail "arduino-cli was not found in PATH."

log "Building the customized eight-peer ESP32-C3 core"
"$SCRIPT_DIR/build-custom-arduino-core.sh"

if ((COMPILE_FIRMWARE == 0)); then
  log "Core-only setup requested; firmware compilation skipped."
  exit 0
fi

if [[ -n "${ARDUINO_SKETCHBOOK_DIR:-}" ]]; then
  SKETCHBOOK_DIR="$ARDUINO_SKETCHBOOK_DIR"
else
  SKETCHBOOK_DIR="$("$ARDUINO_CLI_PATH" config get directories.user 2>/dev/null | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' || true)"
  if [[ -z "$SKETCHBOOK_DIR" || "$SKETCHBOOK_DIR" == "null" || "$SKETCHBOOK_DIR" == *"Error:"* ]]; then
    if [[ "$(uname -s)" == "Darwin" ]]; then SKETCHBOOK_DIR="$HOME/Documents/Arduino"; else SKETCHBOOK_DIR="$HOME/Arduino"; fi
  fi
fi
export ARDUINO_DIRECTORIES_USER="$SKETCHBOOK_DIR"

if [[ ! -f "$DRIVER_LIB/Dw3000/library.properties" ]]; then
  [[ -n "$REPO_ROOT" && -f "$REPO_ROOT/.gitmodules" ]] || fail "Run this script from a clone of dwm3000-esp32-interface with its .gitmodules file present."
  log "Initializing the DWM3000 driver submodule"
  git -C "$REPO_ROOT" submodule update --init --recursive -- dwm3000-firmware/UwbNode/lib/Makerfabs-ESP32-UWB-DW3000
fi
[[ -f "$DRIVER_LIB/Dw3000/library.properties" ]] || fail "The Makerfabs DWM3000 driver submodule is missing or incomplete: $DRIVER_LIB"

log "Installing the pinned Arduino libraries required by UwbNode"
"$ARDUINO_CLI_PATH" lib update-index
while IFS= read -r dependency || [[ -n "$dependency" ]]; do
  [[ -z "$dependency" || "$dependency" == #* ]] && continue
  "$ARDUINO_CLI_PATH" lib install "$dependency"
  library_name="${dependency%@*}"
  library_version="${dependency#*@}"
  properties="$SKETCHBOOK_DIR/libraries/$library_name/library.properties"
  [[ -s "$properties" ]] || fail "Arduino library $library_name was not installed at $properties."
  grep -Fxq "name=$library_name" "$properties" || fail "Installed Arduino library name does not match $library_name."
  grep -Fxq "version=$library_version" "$properties" || fail "Installed $library_name version is not $library_version."
done < "$LIBRARY_LIST"

log "Compiling UwbNode using the custom core"
"$ARDUINO_CLI_PATH" compile \
  --fqbn "$FQBN" \
  --libraries "$DRIVER_LIB" \
  "$UWB_NODE_DIR"

log "Custom core and UwbNode firmware build completed."
