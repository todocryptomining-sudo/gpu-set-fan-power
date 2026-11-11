#!/bin/bash
# set_fan_curve installer v1.9
# ─────────────────────────────────────────────────────────────────────────────
# v1.7 changes vs v1.6:
#   • critical_temp now accepts 50-110 °C (105/107 allowed for GDDR6X
#     hotspots that normally peak at 100-105 °C).
#   • NEW: set_fan_curve --check-config / --validate
#       - checks every line, unknown keys (typos), missing values,
#         non-numeric values and out-of-range values;
#       - prints the effective values and the exact sensors used by the
#         curve and by the critical lock;
#       - exits 1 when the file must not be applied;
#       - does not need root and does not touch the running service.
#   • restartgpu validates the file before restarting. A config with errors
#     is never applied; the service keeps the previous configuration.
#   • editargpu shows a diff of the changes, validates, and asks before
#     restarting the service. If there are errors it offers to re-edit.
#   • Startup log now prints the curve sensors and the lock sensors
#     explicitly, so it is clear whether the hotspot can force 100 %.
#   • Includes all v1.6 changes:
#       - max_temp=100 accepted (range 50-100), critical_temp independent
#       - curve 0 → max_speed with min_speed as a floor
#       - max_speed (20-100 %), ignored by the critical lock
#       - per-fan state and NVML error checking
#       - fan-at-0 % alerts
#       - MANUAL fan policy re-asserted every 60 s, AUTO on shutdown
#       - /run/set-fan-curve/fans and multi-fan output
#   • Includes all v1.5 features:
#       - setfan manual test, gpuhelp, updategpu
#       - critical lock overrides manual mode
#   • Includes all v1.4.9 fixes:
#       - critical lock uses the same sensors as the curve when
#         safety_all_sensors=0 (no more permanent 100 % with hotspot)
#       - safety_all_sensors config key (default 0)
#   • Includes all v1.4.8 fixes:
#       - ACTUAL_HOME resolved before use
#       - BAR0 read from /sys/.../resource (text), not resource0 (binary)
#       - PCI bus ID with function .0 (not pciDeviceId)
#       - compile errors fixed
#       - commands installed in the real ~/.local/bin
#       - compiles before stopping/removing the previous installation
#       - old udev rule removed (driver device permissions untouched)
#   • VRAM: BAR0 + 0xE2A8, (raw & 0xFFF) / 0x20  (GA104 uses 0xEE50)
#   • Hotspot: BAR0 + 0x2046C, (raw >> 8) & 0xFF
#   • Daemon publishes temps to /run/set-fan-curve/temps so vergpu works without root
#   • Critical temperature lock is latching (releases 5 °C below critical)
#   • Restores automatic fan control with nvmlDeviceSetDefaultFanSpeed_v2
#   • Upgrade-safe: preserves ~/set-fan-config and appends new parameters
#   • Never touches NVIDIA drivers, CUDA, Docker or Vast.ai components
# Run with: sudo ./install.sh
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# Configuration
# ─────────────────────────────────────────────────────────────────────────────
CURRENT_VERSION="v1.9"
BINARY_NAME="set_fan_curve"
VERGPU_BINARY="vergpu"
INSTALL_DIR="/usr/local/bin"
CONFIG_DIR="/etc/set-fan-curve"
SERVICE_NAME="set-fan-curve"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
UDEV_RULES_FILE="/etc/udev/rules.d/99-nvidia-gpu-fan.rules"
EXAMPLE_CONFIG="set-fan-config.example"
SOURCE_FILE="set_fan_curve.cpp"
VERGPU_SOURCE="vergpu.cpp"
GPU_TEMPS_SOURCE="gpu_temps.c"
GPU_TEMPS_HEADER="gpu_temps.h"
NVML_MIN="nvml_min.h"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_err()  { echo -e "${RED}[ERROR]${NC} $*"; }

# ─────────────────────────────────────────────────────────────────────────────
# Root check + resolve real user/home BEFORE any path that uses them
# ─────────────────────────────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    log_err "Must be run as root (use sudo)"
    exit 1
fi

ACTUAL_USER="${SUDO_USER:-$USER}"
ACTUAL_HOME="$(getent passwd "$ACTUAL_USER" | cut -d: -f6 || true)"
if [[ -z "${ACTUAL_HOME:-}" || ! -d "$ACTUAL_HOME" ]]; then
    ACTUAL_HOME="$HOME"
fi

USER_CONFIG_FILE="${ACTUAL_HOME}/set-fan-config"
USER_COMMANDS_DIR="${ACTUAL_HOME}/.local/bin"

log_info "Installing for user: $ACTUAL_USER (home: $ACTUAL_HOME)"
log_info "Package version: $CURRENT_VERSION"

# ─────────────────────────────────────────────────────────────────────────────
# Source checks
# ─────────────────────────────────────────────────────────────────────────────
for f in "$SOURCE_FILE" "$VERGPU_SOURCE" "$EXAMPLE_CONFIG" "$NVML_MIN" \
         "$GPU_TEMPS_SOURCE" "$GPU_TEMPS_HEADER"; do
    if [[ ! -f "$f" ]]; then
        log_err "Missing source file: $f"
        log_err "Run this script from the extracted package directory."
        exit 1
    fi
done

# ─────────────────────────────────────────────────────────────────────────────
# Build tools (compiler only; no CUDA/nvcc, no nvml-dev)
# ─────────────────────────────────────────────────────────────────────────────
log_info "Installing build tools (gcc/g++, no CUDA)..."
apt-get update -qq
apt-get install -y -qq build-essential 2>/dev/null || true

for tool in gcc g++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        log_err "$tool not found after installing build-essential"
        exit 1
    fi
done

# ─────────────────────────────────────────────────────────────────────────────
# NVML library (driver ships libnvidia-ml.so.1; headers are not required)
# ─────────────────────────────────────────────────────────────────────────────
NVML_LIB=""
for candidate in \
    /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1 \
    /usr/lib64/libnvidia-ml.so.1 \
    /usr/lib/libnvidia-ml.so.1 \
    /lib/x86_64-linux-gnu/libnvidia-ml.so.1; do
    if [[ -e "$candidate" ]]; then
        NVML_LIB="$candidate"
        break
    fi
done
if [[ -z "$NVML_LIB" ]]; then
    NVML_LIB="$(find /usr/lib /usr/lib64 /lib -name 'libnvidia-ml.so.1' -type f 2>/dev/null | head -1 || true)"
fi
if [[ -z "$NVML_LIB" ]]; then
    log_err "libnvidia-ml.so.1 not found. Is the NVIDIA driver installed?"
    exit 1
fi
log_ok "NVML library: $NVML_LIB"

# ─────────────────────────────────────────────────────────────────────────────
# Compile into a temporary directory FIRST (old install keeps running on failure)
# ─────────────────────────────────────────────────────────────────────────────
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

log_info "Compiling gpu_temps.c (direct PCI register reader)..."
if ! gcc -std=c11 -O2 -Wall -Wextra -c "$GPU_TEMPS_SOURCE" \
        -o "$BUILD_DIR/gpu_temps.o" 2>"$BUILD_DIR/err_c.txt"; then
    log_err "Compilation of $GPU_TEMPS_SOURCE failed:"
    cat "$BUILD_DIR/err_c.txt"
    log_err "Nothing was modified. Fix the error and re-run."
    exit 1
fi
log_ok "gpu_temps.o compiled"

log_info "Compiling $BINARY_NAME..."
if ! g++ -std=c++17 -O2 -Wall -Wextra \
        -o "$BUILD_DIR/$BINARY_NAME" "$SOURCE_FILE" "$BUILD_DIR/gpu_temps.o" \
        "$NVML_LIB" 2>"$BUILD_DIR/err_cpp.txt"; then
    log_err "Compilation of $BINARY_NAME failed:"
    cat "$BUILD_DIR/err_cpp.txt"
    log_err "Nothing was modified. Fix the error and re-run."
    exit 1
fi
log_ok "$BINARY_NAME compiled"

log_info "Compiling $VERGPU_BINARY..."
if ! g++ -std=c++17 -O2 -Wall -Wextra \
        -o "$BUILD_DIR/$VERGPU_BINARY" "$VERGPU_SOURCE" "$BUILD_DIR/gpu_temps.o" \
        "$NVML_LIB" -ldl 2>"$BUILD_DIR/err_vergpu.txt"; then
    log_err "Compilation of $VERGPU_BINARY failed:"
    cat "$BUILD_DIR/err_vergpu.txt"
    log_err "Nothing was modified. Fix the error and re-run."
    exit 1
fi
log_ok "$VERGPU_BINARY compiled"

# ─────────────────────────────────────────────────────────────────────────────
# UPGRADE: clean previous installation (user config is preserved)
# ─────────────────────────────────────────────────────────────────────────────
PREVIOUS_INSTALLED=false
if [[ -f "${INSTALL_DIR}/${BINARY_NAME}" ]] || \
   [[ -f "${INSTALL_DIR}/${VERGPU_BINARY}" ]] || \
   [[ -f "$SERVICE_FILE" ]]; then
    PREVIOUS_INSTALLED=true
fi

if [[ "$PREVIOUS_INSTALLED" = true ]]; then
    log_warn "Previous installation detected — backing up before upgrade"
    BACKUP_DIR="$(mktemp -d /var/backups/set-fan-curve-v1.9-XXXXXXXX)"
    for path in "$SERVICE_FILE" "${INSTALL_DIR}/${BINARY_NAME}" "${INSTALL_DIR}/${VERGPU_BINARY}"; do
        if [[ -f "$path" ]]; then cp -a "$path" "$BACKUP_DIR/"; fi
    done
    for cmd in editargpu restartgpu gpustatus gpusave setfan gpuhelp updategpu; do
        if [[ -f "${INSTALL_DIR}/${cmd}" ]]; then cp -a "${INSTALL_DIR}/${cmd}" "$BACKUP_DIR/"; fi
    done
    if [[ -f "$USER_CONFIG_FILE" ]]; then cp -a "$USER_CONFIG_FILE" "$BACKUP_DIR/set-fan-config"; fi
    log_ok "Backup: $BACKUP_DIR"

    systemctl stop "$SERVICE_NAME" 2>/dev/null || true
    systemctl disable "$SERVICE_NAME" 2>/dev/null || true
    rm -f "$SERVICE_FILE"
    systemctl daemon-reload 2>/dev/null || true

    rm -f "${INSTALL_DIR}/${BINARY_NAME}" "${INSTALL_DIR}/${VERGPU_BINARY}"
    for cmd in editargpu restartgpu gpustatus gpusave setfan gpuhelp updategpu; do
        rm -f "${INSTALL_DIR}/${cmd}"
        rm -f "${USER_COMMANDS_DIR}/${cmd}"
    done

    # v1.4.4–v1.4.7 installed a udev rule that changed /dev/nvidia* to 0660.
    # It is no longer needed (daemon runs as root) and can break other
    # containers/processes that expect the driver's default permissions.
    if [[ -f "$UDEV_RULES_FILE" ]]; then
        rm -f "$UDEV_RULES_FILE"
        udevadm control --reload-rules 2>/dev/null || true
        udevadm trigger 2>/dev/null || true
        log_ok "Removed old udev rule (driver device permissions left untouched)"
    fi

    # Migrate very old config location (~/.config) if needed
    OLD_CONFIG="${ACTUAL_HOME}/.config/set-fan-config"
    if [[ -f "$OLD_CONFIG" && ! -f "$USER_CONFIG_FILE" ]]; then
        log_info "Moving old config to ~/set-fan-config"
        mv "$OLD_CONFIG" "$USER_CONFIG_FILE" 2>/dev/null || true
        chown "$ACTUAL_USER:$ACTUAL_USER" "$USER_CONFIG_FILE" 2>/dev/null || true
    fi

    log_ok "Previous installation cleaned. User config preserved."
fi

# ─────────────────────────────────────────────────────────────────────────────
# Install binaries + commands
# ─────────────────────────────────────────────────────────────────────────────
log_info "Installing binaries to ${INSTALL_DIR}/ ..."
install -m 755 "$BUILD_DIR/$BINARY_NAME" "${INSTALL_DIR}/${BINARY_NAME}"
install -m 755 "$BUILD_DIR/$VERGPU_BINARY" "${INSTALL_DIR}/${VERGPU_BINARY}"
log_ok "Binaries installed"

log_info "Installing GPU commands..."
mkdir -p "$USER_COMMANDS_DIR"
chown "$ACTUAL_USER:$ACTUAL_USER" "$USER_COMMANDS_DIR" 2>/dev/null || true

for cmd in editargpu restartgpu gpustatus gpusave setfan gpuhelp updategpu; do
    if [[ -f "$cmd" ]]; then
        install -m 755 "$cmd" "${INSTALL_DIR}/${cmd}"
        install -m 755 "$cmd" "${USER_COMMANDS_DIR}/${cmd}"
        chown "$ACTUAL_USER:$ACTUAL_USER" "${USER_COMMANDS_DIR}/${cmd}" 2>/dev/null || true
        log_ok "Command: ${INSTALL_DIR}/${cmd}  +  ${USER_COMMANDS_DIR}/${cmd}"
    else
        log_warn "Command not found: $cmd — skipping"
    fi
done

# ─────────────────────────────────────────────────────────────────────────────
# Home README
# ─────────────────────────────────────────────────────────────────────────────
if [[ -f README-gpu-home.md ]]; then
    install -m 644 README-gpu-home.md "${ACTUAL_HOME}/README-gpu.md"
    chown "$ACTUAL_USER:$ACTUAL_USER" "${ACTUAL_HOME}/README-gpu.md" 2>/dev/null || true
    log_ok "Home README: ${ACTUAL_HOME}/README-gpu.md"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Config: fresh install copies the example; upgrade preserves and appends
# ─────────────────────────────────────────────────────────────────────────────
log_info "Setting up configuration..."
mkdir -p "$CONFIG_DIR"
install -m 644 "$EXAMPLE_CONFIG" "${CONFIG_DIR}/set-fan-config.example"

if [[ ! -f "$USER_CONFIG_FILE" ]]; then
    install -m 644 "$EXAMPLE_CONFIG" "$USER_CONFIG_FILE"
    chown "$ACTUAL_USER:$ACTUAL_USER" "$USER_CONFIG_FILE" 2>/dev/null || true
    log_ok "User config created: $USER_CONFIG_FILE"
else
    log_info "Existing config found — preserving your values..."
    cp -a "$USER_CONFIG_FILE" "${USER_CONFIG_FILE}.bak-${CURRENT_VERSION}-$(date +%Y%m%d%H%M%S)"
    chown "$ACTUAL_USER:$ACTUAL_USER" "${USER_CONFIG_FILE}.bak-${CURRENT_VERSION}-"* 2>/dev/null || true
    log_ok "Configuration backup created"

    EXAMPLE_KEYS="$(grep -E '^[A-Za-z_][A-Za-z0-9_]*=' "$EXAMPLE_CONFIG" | cut -d= -f1 || true)"
    EXISTING_KEYS="$(grep -E '^[A-Za-z_][A-Za-z0-9_]*=' "$USER_CONFIG_FILE" | cut -d= -f1 || true)"

    NEW_KEYS=""
    for key in $EXAMPLE_KEYS; do
        if ! echo "$EXISTING_KEYS" | grep -qx "$key"; then
            # Absence preserves v1.7 enforce semantics for an existing limit.
            if [[ "$key" == power_limit_mode ]]; then continue; fi
            NEW_KEYS="$NEW_KEYS $key"
        fi
    done

    if [[ -n "$NEW_KEYS" ]]; then
        log_warn "New parameters: $NEW_KEYS"
        {
            echo ""
            echo "# ─────────────────────────────────────────────────────────────────────"
            echo "# Parameters added by $CURRENT_VERSION installer"
            echo "# Reference with full comments: ${CONFIG_DIR}/set-fan-config.example"
            echo "# ─────────────────────────────────────────────────────────────────────"
            for key in $NEW_KEYS; do
                echo ""
                awk -v k="$key" '
                    { line[NR] = $0 }
                    $0 ~ "^" k "=" { keyline = NR }
                    END {
                        if (keyline == 0) exit
                        start = keyline
                        while (start > 1 && line[start-1] ~ /^#/) start--
                        for (i = start; i <= keyline; i++) print line[i]
                    }
                ' "$EXAMPLE_CONFIG"
            done
        } >> "$USER_CONFIG_FILE"
        chown "$ACTUAL_USER:$ACTUAL_USER" "$USER_CONFIG_FILE" 2>/dev/null || true
        log_ok "New parameters appended to $USER_CONFIG_FILE"
    else
        log_ok "User config already up to date"
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# Systemd service (root is required for /dev/mem and NVML fan control)
# ─────────────────────────────────────────────────────────────────────────────
log_info "Creating systemd service..."
cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=GPU Fan Curve Controller ${CURRENT_VERSION} (NVML + direct PCI temperatures)
After=systemd-modules-load.service nvidia-persistenced.service
Wants=nvidia-persistenced.service
StartLimitIntervalSec=300
StartLimitBurst=5

[Service]
Type=simple
User=root
Group=root
ExecStart=${INSTALL_DIR}/${BINARY_NAME}
Restart=on-failure
RestartSec=10
TimeoutStopSec=15
RuntimeDirectory=set-fan-curve
RuntimeDirectoryMode=0755

Environment=HOME=${ACTUAL_HOME}
Environment=USER=${ACTUAL_USER}
Environment=LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu

StandardOutput=journal
StandardError=journal
SyslogIdentifier=set-fan-curve

[Install]
WantedBy=multi-user.target
EOF
log_ok "Service file: $SERVICE_FILE"

# ─────────────────────────────────────────────────────────────────────────────
# Enable + start
# ─────────────────────────────────────────────────────────────────────────────
systemctl daemon-reload
systemctl enable "$SERVICE_NAME" >/dev/null
systemctl start "$SERVICE_NAME" || true

sleep 3

if systemctl is-active --quiet "$SERVICE_NAME"; then
    echo
    echo "════════════════════════════════════════════════════════════════════"
    echo "  GPU Fan Curve Controller ${CURRENT_VERSION} — Installation complete"
    echo "════════════════════════════════════════════════════════════════════"
    echo
    echo "  Binaries:    ${INSTALL_DIR}/${BINARY_NAME}  ${INSTALL_DIR}/${VERGPU_BINARY}"
    echo "  Commands:    editargpu  restartgpu  gpustatus  gpusave  setfan  gpuhelp  updategpu"
    echo "  Config:      ${USER_CONFIG_FILE}"
    echo "  Home README: ${ACTUAL_HOME}/README-gpu.md"
    echo
    echo "  Quick start:"
    echo "    vergpu          → live monitor (Core / VRAM / Hotspot / Fans)"
    echo "    gpustatus       → quick status"
    echo "    setfan 70       → hold fans at 70 % (Ctrl+C to cancel)"
    echo "    setfan auto     → cancel the manual test"
    echo "    gpuhelp         → list all commands"
    echo "    sudo updategpu  → update from GitHub (keeps your config)"
    echo "    editargpu       → edit config (shows diff, validates, confirms)"
    echo "    restartgpu      → validate and apply config changes"
    echo "    ${INSTALL_DIR}/${BINARY_NAME} --check-config ~/set-fan-config"
    echo "                    → check the config without restarting"
    echo
    echo "  Verify PCI temperatures:"
    echo "    journalctl -u ${SERVICE_NAME} -n 30 --no-pager | grep -i temp"
    echo
    echo "  Verify per-fan speeds:"
    echo "    cat /run/set-fan-curve/fans"
    echo
    echo "  Logs:"
    echo "    journalctl -u ${SERVICE_NAME} -f"
    echo
    echo "  Uninstall:"
    echo "    sudo ./uninstall.sh   (keeps ~/set-fan-config)"
    echo "════════════════════════════════════════════════════════════════════"
else
    log_err "Service failed to start"
    echo
    echo "Recent logs:"
    journalctl -u "$SERVICE_NAME" -n 30 --no-pager || true
    echo
    echo "Manual test as root:"
    echo "  sudo ${INSTALL_DIR}/${BINARY_NAME}"
    exit 1
fi
