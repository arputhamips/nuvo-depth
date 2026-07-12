#!/usr/bin/env bash
set -u

section() {
    printf '\n[%s]\n' "$1"
}

section kernel
uname -a

section loaded_modules
lsmod | grep -E 'imx296|rp1_cfe' || true
modinfo imx296 2>/dev/null | grep -E '^(filename|version|parm):' || true

section development_packages
pkg-config --modversion libcamera 2>/dev/null || echo 'libcamera development package: missing'
pkg-config --modversion opencv4 2>/dev/null || echo 'opencv4 development package: missing'
command -v cmake || true
command -v g++ || true

section upstream_trigger_parameter
if [[ -r /sys/module/imx296/parameters/trigger_mode ]]; then
    printf 'trigger_mode='
    cat /sys/module/imx296/parameters/trigger_mode
else
    echo 'missing: /sys/module/imx296/parameters/trigger_mode'
fi

section boot_camera_configuration
grep -nE 'camera_auto_detect|dtoverlay=.*imx296|always-on' \
    /boot/firmware/config.txt /boot/config.txt 2>/dev/null || true

section cameras
rpicam-hello --list-cameras 2>/dev/null || \
    libcamera-hello --list-cameras 2>/dev/null || true

section i2c_buses
i2cdetect -l 2>/dev/null || true

section gpio17
pinctrl get 17 2>/dev/null || true
gpioinfo gpiochip0 2>/dev/null | grep -E 'line[[:space:]]+17:|GPIO17' || true

section camera_processes
pgrep -af 'rpicam|libcamera|python.*camera|profile_cpu|focus_stream|depth' || true
