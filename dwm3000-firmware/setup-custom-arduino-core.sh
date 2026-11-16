#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
FIRMWARE_DIR="$SCRIPT_DIR"
UWB_NODE_DIR="$FIRMWARE_DIR/UwbNode"
REPO_ROOT="$(git -C "$FIRMWARE_DIR" rev-parse --show-toplevel 2>/dev/null || true)"

readonly BUILDER_REPO="https://github.com/PladsElsker/esp32-arduino-lib-builder.git"
readonly BUILDER_COMMIT="165a3d6e094266eb8c54c96732c0f0cb3e1b3e84"
readonly ARDUINO_REPO="https://github.com/espressif/arduino-esp32.git"
readonly ARDUINO_BRANCH="idf-release/v5.5"
readonly IDF_BRANCH="release/v5.5"
readonly FQBN="esp32:esp32:esp32c3:CDCOnBoot=cdc,JTAGAdapter=builtin"
readonly SUBMODULE_PATH="dwm3000-firmware/UwbNode/lib/Makerfabs-ESP32-UWB-DW3000"

COMPILE_FIRMWARE=1

log() {
  printf '\n==> %s\n' "$*"
}

fail() {
  printf '\nERROR: %s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<USAGE
Usage: $0 [--core-only] [--help]

Build and install the customized ESP32-C3 Arduino core, verify that Arduino CLI
selects it, and compile the UWB Node firmware.

Environment overrides:
  ARDUINO_CLI                         Arduino CLI executable (default: arduino-cli)
  ARDUINO_SKETCHBOOK_DIR               Arduino CLI directories.user
  ESP32_ARDUINO_CORE_DIR               Manual Arduino-ESP32 source directory
  ESP32_ARDUINO_LIB_BUILDER_DIR         Library builder checkout directory
  ESP32_BUILDER_PYTHON_ENV              Python virtual environment directory

--core-only builds and installs the custom core but skips the firmware compile.
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
[[ -n "$ARDUINO_CLI_PATH" ]] || fail "arduino-cli was not found in PATH. Install Arduino CLI first."

check_commands() {
  local missing=()
  local command_name
  for command_name in git python3 cmake ninja ccache curl wget jq make gcc g++ flex bison gperf; do
    command -v "$command_name" >/dev/null 2>&1 || missing+=("$command_name")
  done

  case "$(uname -s)" in
    Linux)
      command -v realpath >/dev/null 2>&1 || missing+=("realpath")
      case "$(uname -m)" in
        x86_64|i686|armv7l) ;;
        *) fail "The pinned library builder does not support Linux architecture $(uname -m). Use x86_64, i686, or armv7l Linux." ;;
      esac
      ;;
    Darwin)
      for command_name in gsed gawk grealpath; do
        command -v "$command_name" >/dev/null 2>&1 || missing+=("$command_name")
      done
      ;;
    *) fail "This script supports Linux and macOS. Use WSL on Windows." ;;
  esac

  if (("${#missing[@]}")); then
    printf 'Missing build prerequisites: %s\n' "${missing[*]}" >&2
    case "$(uname -s)" in
      Linux)
        printf '%s\n' 'Ubuntu/Debian: sudo apt-get install git wget curl libssl-dev libncurses-dev flex bison gperf python3 python3-venv cmake ninja-build ccache jq build-essential' >&2
        ;;
      Darwin)
        printf '%s\n' 'macOS: install Xcode Command Line Tools, then run: brew install git wget curl cmake ninja ccache jq flex bison gperf gnu-sed gawk coreutils' >&2
        ;;
    esac
    exit 1
  fi

  python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' || \
    fail "Python 3.9 or newer is required."
  python3 -c 'import venv' >/dev/null 2>&1 || \
    fail "Python venv support is missing. On Ubuntu/Debian install python3-venv."
}

canonicalize_target() {
  local path="$1"
  mkdir -p "$(dirname "$path")"
  printf '%s/%s\n' "$(cd "$(dirname "$path")" && pwd -P)" "$(basename "$path")"
}

check_git_origin() {
  local directory="$1"
  local expected_fragment="$2"
  local origin
  git -C "$directory" rev-parse --git-dir >/dev/null 2>&1 || \
    fail "$directory exists but is not a Git checkout. Move it aside or set the corresponding directory override."
  origin="$(git -C "$directory" remote get-url origin 2>/dev/null || true)"
  [[ "$origin" == *"$expected_fragment"* ]] || \
    fail "$directory has unexpected origin '$origin' (expected '$expected_fragment')."
}

check_core_worktree() {
  local status line path
  status="$(git -C "$CORE_DIR" status --porcelain --untracked-files=no)"
  while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    path="${line:3}"
    if [[ "$path" != "package/package_esp32_index.template.json" ]]; then
      fail "The Arduino core has local tracked changes in '$path'. Commit or revert them before running this setup script."
    fi
  done <<< "$status"
}

check_commands

if [[ -n "${ARDUINO_SKETCHBOOK_DIR:-}" ]]; then
  SKETCHBOOK_DIR="$ARDUINO_SKETCHBOOK_DIR"
else
  SKETCHBOOK_DIR="$("$ARDUINO_CLI_PATH" config get directories.user 2>/dev/null | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' || true)"
  if [[ -z "$SKETCHBOOK_DIR" || "$SKETCHBOOK_DIR" == "null" || "$SKETCHBOOK_DIR" == *"Error:"* ]]; then
    if [[ "$(uname -s)" == "Darwin" ]]; then
      SKETCHBOOK_DIR="$HOME/Documents/Arduino"
    else
      SKETCHBOOK_DIR="$HOME/Arduino"
    fi
  fi
fi

if [[ -n "${ESP32_ARDUINO_CORE_DIR:-}" ]]; then
  CORE_DIR="$(canonicalize_target "$ESP32_ARDUINO_CORE_DIR")"
else
  CORE_DIR="$(canonicalize_target "$SKETCHBOOK_DIR/hardware/espressif/esp32")"
fi
BUILDER_DIR="$(canonicalize_target "${ESP32_ARDUINO_LIB_BUILDER_DIR:-$HOME/esp32-arduino-lib-builder}")"
PYTHON_ENV_DIR="$(canonicalize_target "${ESP32_BUILDER_PYTHON_ENV:-${XDG_CACHE_HOME:-$HOME/.cache}/esp32-arduino-lib-builder-venv}")"

log "Arduino CLI: $ARDUINO_CLI_PATH"
log "Sketchbook: $SKETCHBOOK_DIR"
log "Arduino-ESP32 core: $CORE_DIR"
log "Library builder: $BUILDER_DIR"

if [[ -e "$CORE_DIR" ]]; then
  check_git_origin "$CORE_DIR" "espressif/arduino-esp32"
  core_branch="$(git -C "$CORE_DIR" branch --show-current)"
  [[ "$core_branch" == "$ARDUINO_BRANCH" ]] || \
    fail "The existing Arduino core is on '$core_branch', expected '$ARDUINO_BRANCH'. Move it aside or set ESP32_ARDUINO_CORE_DIR to a fresh directory."
  check_core_worktree
  submodule_status="$(git -C "$CORE_DIR" submodule status --recursive)"
  if grep -q '^[+U]' <<< "$submodule_status"; then
    fail "The Arduino core has a submodule checked out at a non-recorded revision or in conflict. Resolve it before continuing."
  fi
  if grep -q '^-' <<< "$submodule_status"; then
    log "Initializing Arduino-ESP32 submodules"
    git -C "$CORE_DIR" submodule update --init --recursive
  fi
else
  log "Cloning Arduino-ESP32 ($ARDUINO_BRANCH)"
  git clone --branch "$ARDUINO_BRANCH" --recurse-submodules "$ARDUINO_REPO" "$CORE_DIR"
fi

if [[ -e "$BUILDER_DIR" ]]; then
  check_git_origin "$BUILDER_DIR" "PladsElsker/esp32-arduino-lib-builder"
  [[ -z "$(git -C "$BUILDER_DIR" status --porcelain --untracked-files=no)" ]] || \
    fail "The builder checkout has local tracked changes: $BUILDER_DIR"
else
  log "Cloning the ESP32 Arduino library builder"
  git clone "$BUILDER_REPO" "$BUILDER_DIR"
fi

log "Checking out the pinned builder revision"
git -C "$BUILDER_DIR" fetch origin --tags
git -C "$BUILDER_DIR" checkout --detach "$BUILDER_COMMIT"

log "Creating the builder's isolated Python environment"
python3 -m venv "$PYTHON_ENV_DIR"
PYTHON="$PYTHON_ENV_DIR/bin/python"
"$PYTHON" -m pip install --upgrade pip
"$PYTHON" -m pip install setuptools pyserial click future wheel cryptography pyparsing pyelftools
export PATH="$PYTHON_ENV_DIR/bin:$PATH"

log "Synchronizing the builder's Arduino-ESP32 source branch"
(
  cd "$BUILDER_DIR"
  AR_SOURCE_BRANCH="$ARDUINO_BRANCH" IDF_BRANCH="$IDF_BRANCH" bash ./tools/install-arduino.sh
)
BUILDER_ARDUINO_SHA="$(git -C "$BUILDER_DIR/components/arduino" rev-parse HEAD)"
CORE_SHA="$(git -C "$CORE_DIR" rev-parse HEAD)"
[[ "$CORE_SHA" == "$BUILDER_ARDUINO_SHA" ]] || \
  fail "The manual Arduino core revision ($CORE_SHA) does not match the builder source ($BUILDER_ARDUINO_SHA). Move '$CORE_DIR' aside and rerun this script to create a matching checkout."

log "Installing the Arduino-ESP32 platform tools"
"$PYTHON" "$CORE_DIR/tools/get.py"

log "Building and installing the ESP32-C3 libraries with the BLE configuration"
(
  cd "$BUILDER_DIR"
  export AR_SOURCE_BRANCH="$ARDUINO_BRANCH"
  export IDF_BRANCH="$IDF_BRANCH"
  bash ./build.sh -A "$ARDUINO_BRANCH" -I "$IDF_BRANCH" -t esp32c3 -c "$CORE_DIR"
)
BUILDER_ARDUINO_SHA_AFTER="$(git -C "$BUILDER_DIR/components/arduino" rev-parse HEAD)"
[[ "$CORE_SHA" == "$BUILDER_ARDUINO_SHA_AFTER" ]] || \
  fail "The builder's Arduino source branch moved during the build. Recreate the manual core checkout and rerun this script."

SDK_DIR="$CORE_DIR/tools/esp32-arduino-libs/esp32c3"
SDKCONFIG="$SDK_DIR/sdkconfig"
[[ -f "$SDKCONFIG" ]] || fail "The builder did not produce $SDKCONFIG"
grep -Eq '^CONFIG_BT_NIMBLE_MAX_CONNECTIONS=8$' "$SDKCONFIG" || \
  fail "The generated sdkconfig does not set CONFIG_BT_NIMBLE_MAX_CONNECTIONS=8"
grep -Eq '^CONFIG_BT_CTRL_BLE_MAX_ACT=9$' "$SDKCONFIG" || \
  fail "The generated sdkconfig does not set CONFIG_BT_CTRL_BLE_MAX_ACT=9"

header_ok=0
while IFS= read -r header; do
  if grep -Eq '^#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 8$' "$header" && \
     grep -Eq '^#define CONFIG_BT_CTRL_BLE_MAX_ACT 9$' "$header"; then
    header_ok=1
    log "Verified generated SDK header: $header"
  fi
done < <(find "$SDK_DIR" -path '*/include/sdkconfig.h' -type f -print)
((header_ok == 1)) || fail "No generated memory-variant sdkconfig.h contains both expected BLE values."

log "Verifying Arduino CLI resolves the custom core"
BOARD_PROPERTIES="$("$ARDUINO_CLI_PATH" board details --fqbn "$FQBN" --show-properties=expanded)" || \
  fail "Arduino CLI cannot resolve $FQBN. Verify the manual core installation."
SELECTED_PLATFORM="$(printf '%s\n' "$BOARD_PROPERTIES" | sed -n 's/^runtime\.platform\.path=//p' | tail -n 1)"
[[ -n "$SELECTED_PLATFORM" ]] || \
  fail "Arduino CLI did not report runtime.platform.path. Update Arduino CLI to a current release."
SELECTED_PLATFORM="$(cd "$SELECTED_PLATFORM" && pwd -P)"
[[ "$SELECTED_PLATFORM" == "$CORE_DIR" ]] || {
  printf 'Arduino CLI selected: %s\nCustom core:          %s\n' "$SELECTED_PLATFORM" "$CORE_DIR" >&2
  fail "Arduino CLI is not selecting the custom core. If the Boards Manager ESP32 core is installed, uninstall that duplicate with 'arduino-cli core uninstall esp32:esp32' and rerun this script."
}

if ((COMPILE_FIRMWARE)); then
  if [[ ! -f "$DRIVER_LIB/Dw3000/library.properties" ]]; then
    if [[ -n "$REPO_ROOT" && -f "$REPO_ROOT/.gitmodules" ]]; then
      log "Initializing the DWM3000 driver submodule"
      git -C "$REPO_ROOT" submodule update --init --recursive -- "$SUBMODULE_PATH"
    fi
  fi
  [[ -f "$DRIVER_LIB/Dw3000/library.properties" ]] || \
    fail "The Makerfabs DWM3000 library submodule is missing. Clone this repository with submodules or initialize $SUBMODULE_PATH."

  log "Compiling the firmware with the verified custom core"
  "$ARDUINO_CLI_PATH" compile \
    --fqbn "$FQBN" \
    --libraries "$DRIVER_LIB" \
    "$UWB_NODE_DIR"
fi

log "Custom ESP32-C3 core is ready"
printf 'Host connections:      8\nController activities:  9\nCore path:              %s\n' "$CORE_DIR"
if ((COMPILE_FIRMWARE)); then
  printf 'Firmware sketch:        %s\n' "$UWB_NODE_DIR"
fi
