#!/usr/bin/env bash
# Remove cmp_rebar_shim from the running kernel and the initramfs.
set -euo pipefail

DRY_RUN=0
case "${1:-}" in
    --dry-run) DRY_RUN=1 ;;
    "") ;;
    *) echo "usage: $0 [--dry-run]" >&2; exit 2 ;;
esac

KVER="$(uname -r)"
DEST="/lib/modules/$KVER/extra/cmp_rebar_shim.ko"
CONF="/etc/modprobe.d/cmp-rebar-shim.conf"
INITRAMS="/etc/initramfs-tools/modules"

if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    echo "error: run as root (sudo $0)" >&2
    exit 1
fi

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '  + %s\n' "$*"
    else
        "$@"
    fi
}

run rm -f "$CONF"

if [ -f "$INITRAMS" ] \
   && grep -qE '^[[:space:]]*cmp_rebar_shim[[:space:]]*$' "$INITRAMS"; then
    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  + remove cmp_rebar_shim from $INITRAMS"
    else
        sed -i '/^[[:space:]]*cmp_rebar_shim[[:space:]]*$/d' "$INITRAMS"
        echo "removed cmp_rebar_shim from $INITRAMS"
    fi
fi

run rm -f "$DEST"
run depmod -a "$KVER"

if command -v update-initramfs >/dev/null 2>&1; then
    run update-initramfs -u -k "$KVER"
else
    echo "warning: update-initramfs not found; update the initramfs manually" >&2
fi

if [ "$DRY_RUN" -eq 1 ]; then
    echo
    echo "dry-run: no changes made"
    exit 0
fi

if lsmod | grep -q '^cmp_rebar_shim '; then
    echo
    echo "note: cmp_rebar_shim is currently loaded. 'rmmod cmp_rebar_shim'"
    echo "      only unloads the code; the BAR layout stays until reboot."
fi

echo
echo "Removed. Reboot to restore the firmware BAR layout."
