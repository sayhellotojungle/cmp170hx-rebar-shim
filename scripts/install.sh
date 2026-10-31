#!/usr/bin/env bash
# Install cmp_rebar_shim for the running kernel and add it to the initramfs.
set -euo pipefail

DRY_RUN=0
case "${1:-}" in
    --dry-run) DRY_RUN=1 ;;
    "") ;;
    *) echo "usage: $0 [--dry-run]" >&2; exit 2 ;;
esac

KVER="$(uname -r)"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MOD="$HERE/cmp_rebar_shim.ko"
DEST="/lib/modules/$KVER/extra/cmp_rebar_shim.ko"
CONF="/etc/modprobe.d/cmp-rebar-shim.conf"
INITRAMS="/etc/initramfs-tools/modules"

if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    echo "error: run as root (sudo $0)" >&2
    exit 1
fi

if [ ! -f "$MOD" ]; then
    echo "error: $MOD not found; run 'make' first" >&2
    exit 1
fi

if [ ! -d "/lib/modules/$KVER/build" ]; then
    echo "error: kernel headers missing for $KVER" >&2
    echo "       install the headers for your running kernel and rebuild" >&2
    exit 1
fi

if [ ! -f "$INITRAMS" ]; then
    echo "warning: $INITRAMS not found" >&2
    echo "         this installer targets initramfs-tools; see docs/INSTALL.md" >&2
fi

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '  + %s\n' "$*"
    else
        "$@"
    fi
}

# Refuse an initramfs that would load NVIDIA before the shim.
if [ -f "$INITRAMS" ]; then
    shim_line="$(grep -nE '^[[:space:]]*cmp_rebar_shim[[:space:]]*$' "$INITRAMS" \
        | head -n1 | cut -d: -f1 || true)"
    nvidia_line="$(grep -nE '^[[:space:]]*nvidia([[:space:]]|$)' "$INITRAMS" \
        | head -n1 | cut -d: -f1 || true)"
    if [ -n "$nvidia_line" ] \
       && { [ -z "$shim_line" ] || [ "$nvidia_line" -lt "$shim_line" ]; }; then
        echo "error: $INITRAMS lists nvidia before cmp_rebar_shim." >&2
        echo "       Remove nvidia from the initramfs (recommended); the shim" >&2
        echo "       must load before NVIDIA probes the GPUs." >&2
        exit 1
    fi
fi

echo "kernel: $KVER"
echo "module: $MOD"
echo

run install -D -m 0644 "$MOD" "$DEST"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "  + write $CONF"
else
    cat > "$CONF" <<'EOF'
options cmp_rebar_shim mode=1 commit=1
EOF
fi

if [ -f "$INITRAMS" ]; then
    if grep -qxF 'cmp_rebar_shim' "$INITRAMS"; then
        echo "initramfs: cmp_rebar_shim already listed"
    elif [ "$DRY_RUN" -eq 1 ]; then
        echo "  + append cmp_rebar_shim to $INITRAMS"
    else
        printf '\ncmp_rebar_shim\n' >> "$INITRAMS"
        echo "initramfs: appended cmp_rebar_shim to $INITRAMS"
    fi
fi

run depmod -a "$KVER"

if command -v update-initramfs >/dev/null 2>&1; then
    run update-initramfs -u -k "$KVER"
else
    echo "warning: update-initramfs not found; integrate the module manually" >&2
    echo "         (docs/INSTALL.md, dracut section)" >&2
fi

if [ "$DRY_RUN" -eq 1 ]; then
    echo
    echo "dry-run: no changes made"
    exit 0
fi

echo
if command -v lsinitramfs >/dev/null 2>&1 \
   && [ -f "/boot/initrd.img-$KVER" ]; then
    if lsinitramfs "/boot/initrd.img-$KVER" | grep -q 'cmp_rebar_shim.ko'; then
        echo "initramfs check: OK (cmp_rebar_shim.ko present)"
    else
        echo "initramfs check: WARNING: module not found in /boot/initrd.img-$KVER" >&2
        echo "check your bootloader/initramfs configuration" >&2
    fi
fi

if grep -Rqs 'RMPcieP2PType=1' /etc/modprobe.d/; then
    echo "P2P check: cmpunlocker --p2p registry dwords found"
else
    echo "P2P check: no RMPcieP2PType=1 found"
    echo "           the shim works without P2P, but BAR1-P2P needs the"
    echo "           cmpunlocker --p2p driver (see docs/CMPUNLOCKER.md)"
fi

echo
echo "Installed for the next boot. A cold reboot is required so the shim"
echo "runs from the initramfs before the NVIDIA driver probes the GPUs."
echo "After reboot run: sudo ./scripts/verify.sh"
