#!/usr/bin/env bash
set -euo pipefail

APP_NAME="trlcd"
BIN_NAME="trlcd_libusb"
VID_HEX="0416"
PID_HEX="5302"

# Defaults
MODE="user"             # or "system"
PREFIX_USER="${HOME}/.local"
CONF_USER="${HOME}/.config/${APP_NAME}"
BIN_DIR_USER="${PREFIX_USER}/bin"
SERVICE_NAME="${APP_NAME}.service"
ENABLE_LINGER="no"
NO_BUILD="no"
DRY_RUN="no"

# Paths (system mode)
BIN_DIR_SYSTEM="/usr/local/bin"
CONF_SYSTEM="/etc/${APP_NAME}"
UDEV_RULE="/etc/udev/rules.d/99-${APP_NAME}.rules"
SERVICE_SYSTEM="/etc/systemd/system/${SERVICE_NAME}"

# Detect repo root (where sources live)
REPO_DIR="$(pwd)"
SRC_MAIN="${REPO_DIR}/trlcd_libusb.c"
SRC_LODE="${REPO_DIR}/lodepng.c"
HDR_LODE="${REPO_DIR}/lodepng.h"
HDR_STBI="${REPO_DIR}/stb_image.h"
HDR_STBT="${REPO_DIR}/stb_truetype.h"
LAYOUT_SRC="${REPO_DIR}/layout.cfg"

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  --user                 Install as per-user service (default)
  --system               Install as system service (root)
  --enable-linger        For --user: enable linger so it starts at boot
  --no-build             Skip building (use existing ${BIN_NAME} in repo)
  --service-name NAME    Override systemd service name (default: ${SERVICE_NAME})
  --dry-run              Print actions only
  --uninstall            Remove service, rules, and installed files
  -h, --help             Show this help

Examples:
  $0 --user
  sudo $0 --system
EOF
}

say() { echo -e "==> $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

need_cmd() { command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"; }

pkg_install() {
  # $@: packages to install
  if [[ "${DRY_RUN}" == "yes" ]]; then
    say "[dry-run] would install packages: $*"
    return 0
  fi
  if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update -y
    sudo apt-get install -y "$@"
  elif command -v dnf >/dev/null 2>&1; then
    sudo dnf install -y "$@"
  elif command -v zypper >/dev/null 2>&1; then
    sudo zypper install -y "$@"
  elif command -v pacman >/dev/null 2>&1; then
    sudo pacman -Sy --noconfirm "$@"
  else
    die "Unsupported distro; install these packages manually: $*"
  fi
}

ensure_deps() {
  say "Checking build dependencies"
  need_cmd gcc
  need_cmd pkg-config || true
  # libusb dev headers (best-effort check)
  if ! echo '#include <libusb-1.0/libusb.h>' | cpp -H -o /dev/null 2>&1 | grep -q libusb ; then
    say "Installing libusb development headers"
    pkg_install libusb-1.0-0-dev || pkg_install libusb-devel || pkg_install libusb
  fi
}

build_binary() {
  if [[ "${NO_BUILD}" == "yes" ]]; then
    say "Skipping build (--no-build)"
    return 0
  fi
  [[ -f "${SRC_MAIN}" ]] || die "Source not found: ${SRC_MAIN}"
  [[ -f "${SRC_LODE}" && -f "${HDR_LODE}" ]] || die "lodepng.c/h not found in repo"
  [[ -f "${HDR_STBI}" && -f "${HDR_STBT}" ]] || die "stb headers not found in repo"
  say "Building ${BIN_NAME}"
  local cmd=(gcc -O2 -Wall "${SRC_MAIN}" "${SRC_LODE}" -lusb-1.0 -lm -o "${REPO_DIR}/${BIN_NAME}")
  echo "    ${cmd[*]}"
  if [[ "${DRY_RUN}" != "yes" ]]; then
    "${cmd[@]}"
  fi
}

install_user() {
  local bindir="${BIN_DIR_USER}"
  local confdir="${CONF_USER}"
  local service_dir="${HOME}/.config/systemd/user"
  local service_path="${service_dir}/${SERVICE_NAME}"

  say "Installing (user mode)"
  say "Creating ${bindir} and ${confdir}"
  [[ "${DRY_RUN}" == "yes" ]] || mkdir -p "${bindir}" "${confdir}" "${service_dir}"

  say "Copying binary to ${bindir}/${BIN_NAME}"
  [[ "${DRY_RUN}" == "yes" ]] || install -m 0755 "${REPO_DIR}/${BIN_NAME}" "${bindir}/${BIN_NAME}"

  if [[ -f "${LAYOUT_SRC}" ]]; then
    say "Copying layout.cfg to ${confdir}/layout.cfg (won't overwrite existing)"
    if [[ "${DRY_RUN}" != "yes" ]]; then
      [[ -f "${confdir}/layout.cfg" ]] || install -m 0644 "${LAYOUT_SRC}" "${confdir}/layout.cfg"
    fi
  fi

  # udev rule for device access via uaccess
  say "Installing udev rule (requires sudo)"
  if [[ "${DRY_RUN}" != "yes" ]]; then
    echo "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"${VID_HEX}\", ATTR{idProduct}==\"${PID_HEX}\", TAG+=\"uaccess\"" | sudo tee "${UDEV_RULE}" >/dev/null
    sudo udevadm control --reload-rules
    sudo udevadm trigger
  fi

  say "Writing user service ${service_path}"
  local exec="${bindir}/${BIN_NAME}"
  local work="${confdir}"
  local unit="[Unit]
Description=TRLCD USB display compositor (user)
After=default.target

[Service]
Type=simple
WorkingDirectory=${work}
ExecStart=${exec}
Restart=always
RestartSec=2
Nice=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=default.target
"
  if [[ "${DRY_RUN}" == "yes" ]]; then
    echo "---- ${service_path} ----"
    echo "${unit}"
    echo "-------------------------"
  else
    printf "%s" "${unit}" > "${service_path}"
  fi

  say "Enabling and starting user service"
  if [[ "${DRY_RUN}" != "yes" ]]; then
    systemctl --user daemon-reload
    systemctl --user enable --now "${SERVICE_NAME}"
  fi

  if [[ "${ENABLE_LINGER}" == "yes" ]]; then
    say "Enabling linger so user service runs at boot"
    [[ "${DRY_RUN}" == "yes" ]] || sudo loginctl enable-linger "${USER}"
  fi

  say "Done. View logs with:  journalctl --user -u ${SERVICE_NAME} -f"
}

install_system() {
  [[ $EUID -eq 0 ]] || die "--system requires root (sudo)"
  local bindir="${BIN_DIR_SYSTEM}"
  local confdir="${CONF_SYSTEM}"
  local service_path="${SERVICE_SYSTEM}"

  say "Installing (system mode)"
  mkdir -p "${bindir}" "${confdir}"

  say "Copying binary to ${bindir}/${BIN_NAME}"
  install -m 0755 "${REPO_DIR}/${BIN_NAME}" "${bindir}/${BIN_NAME}"

  if [[ -f "${LAYOUT_SRC}" ]]; then
    say "Copying layout.cfg to ${confdir}/layout.cfg (won't overwrite existing)"
    [[ -f "${confdir}/layout.cfg" ]] || install -m 0644 "${LAYOUT_SRC}" "${confdir}/layout.cfg"
  fi

  say "Installing udev rule ${UDEV_RULE}"
  echo "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"${VID_HEX}\", ATTR{idProduct}==\"${PID_HEX}\", MODE=\"0660\", GROUP=\"plugdev\"" > "${UDEV_RULE}"
  udevadm control --reload-rules
  udevadm trigger

  say "Writing system service ${service_path}"
  cat > "${service_path}" <<EOF
[Unit]
Description=TRLCD USB display compositor (system)
After=multi-user.target

[Service]
Type=simple
WorkingDirectory=${confdir}
ExecStart=${bindir}/${BIN_NAME}
Restart=always
RestartSec=2
Nice=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

  say "Reloading systemd and enabling service"
  systemctl daemon-reload
  systemctl enable --now "${SERVICE_NAME}"
  say "Done. View logs with:  sudo journalctl -u ${SERVICE_NAME} -f"
}

uninstall_user() {
  local service_dir="${HOME}/.config/systemd/user"
  local service_path="${service_dir}/${SERVICE_NAME}"
  say "Stopping and disabling user service (if present)"
  systemctl --user disable --now "${SERVICE_NAME}" 2>/dev/null || true
  say "Removing ${service_path}"
  rm -f "${service_path}"
  systemctl --user daemon-reload || true
  say "Keeping ${CONF_USER} and ${BIN_DIR_USER}/${BIN_NAME} (remove manually if desired)"
}

uninstall_system() {
  [[ $EUID -eq 0 ]] || die "--system + --uninstall requires root"
  say "Stopping and disabling system service"
  systemctl disable --now "${SERVICE_NAME}" 2>/dev/null || true
  say "Removing service file ${SERVICE_SYSTEM}"
  rm -f "${SERVICE_SYSTEM}"
  systemctl daemon-reload || true
  say "Removing udev rule ${UDEV_RULE}"
  rm -f "${UDEV_RULE}"
  udevadm control --reload-rules
  say "Keeping ${BIN_DIR_SYSTEM}/${BIN_NAME} and ${CONF_SYSTEM} (remove manually if desired)"
}

UNINSTALL="no"

# Parse args
while [[ $# -gt 0 ]]; do
  case "$1" in
    --user) MODE="user";;
    --system) MODE="system";;
    --enable-linger) ENABLE_LINGER="yes";;
    --no-build) NO_BUILD="yes";;
    --service-name) shift; [[ $# -gt 0 ]] || die "--service-name needs a value"; SERVICE_NAME="$1";;
    --dry-run) DRY_RUN="yes";;
    --uninstall) UNINSTALL="yes";;
    -h|--help) usage; exit 0;;
    *) die "Unknown option: $1";;
  esac
  shift
done

if [[ "${UNINSTALL}" == "yes" ]]; then
  if [[ "${MODE}" == "user" ]]; then
    uninstall_user
  else
    uninstall_system
  fi
  exit 0
fi

ensure_deps
build_binary

if [[ "${MODE}" == "user" ]]; then
  install_user
else
  install_system
fi
