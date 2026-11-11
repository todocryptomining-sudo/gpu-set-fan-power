#!/bin/bash
# set_fan_curve uninstaller v1.81
# Run with: sudo ./uninstall.sh
set -euo pipefail

SERVICE_NAME="set-fan-curve"
BINARY_NAMES=("set_fan_curve" "vergpu")
COMMAND_NAMES=("editargpu" "restartgpu" "gpustatus" "gpusave" "setfan" "gpuhelp" "updategpu")
INSTALL_DIR="/usr/local/bin"
CONFIG_DIR="/etc/set-fan-curve"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
UDEV_RULES_FILE="/etc/udev/rules.d/99-nvidia-gpu-fan.rules"
STATUS_DIR="/run/set-fan-curve"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_err()  { echo -e "${RED}[ERROR]${NC} $*"; }

if [[ $EUID -ne 0 ]]; then
    log_err "Must run as root (sudo)"
    exit 1
fi

ACTUAL_USER="${SUDO_USER:-$USER}"
ACTUAL_HOME="$(getent passwd "$ACTUAL_USER" | cut -d: -f6 || true)"
[[ -z "${ACTUAL_HOME:-}" ]] && ACTUAL_HOME="/home/${ACTUAL_USER}"

log_info "Stopping and disabling ${SERVICE_NAME}..."
systemctl stop "$SERVICE_NAME" 2>/dev/null || true
systemctl disable "$SERVICE_NAME" 2>/dev/null || true

log_info "Removing service file..."
rm -f "$SERVICE_FILE"
systemctl daemon-reload 2>/dev/null || true

log_info "Removing binaries..."
for bin in "${BINARY_NAMES[@]}"; do
    rm -f "${INSTALL_DIR}/${bin}"
    log_ok "Removed ${INSTALL_DIR}/${bin}"
done

log_info "Removing GPU commands..."
for cmd in "${COMMAND_NAMES[@]}"; do
    rm -f "${INSTALL_DIR}/${cmd}"
    rm -f "${ACTUAL_HOME}/.local/bin/${cmd}"
    log_ok "Removed ${cmd}"
done

if [[ -f "$UDEV_RULES_FILE" ]]; then
    log_info "Removing old udev rule..."
    rm -f "$UDEV_RULES_FILE"
    udevadm control --reload-rules 2>/dev/null || true
    udevadm trigger 2>/dev/null || true
fi

log_info "Removing runtime status directory..."
rm -rf "$STATUS_DIR"

log_info "Removing config directory..."
rm -rf "$CONFIG_DIR"
log_ok "Removed $CONFIG_DIR"

log_info "User config preserved at: ${ACTUAL_HOME}/set-fan-config"
log_info "To remove it too: rm -f ${ACTUAL_HOME}/set-fan-config"

log_ok "Uninstall complete (v1.81)"
