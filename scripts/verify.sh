#!/usr/bin/env bash
# Verify the shim + 64G BAR1 + real CUDA P2P transfers.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0

echo "== shim module =="
if lsmod | grep -q '^cmp_rebar_shim '; then
    echo "loaded"
else
    echo "NOT loaded"
    fail=1
fi
echo

echo "== CMP BARs =="
found=0
for d in /sys/bus/pci/devices/*; do
    [ -r "$d/vendor" ] || continue
    [ "$(cat "$d/vendor" 2>/dev/null)" = "0x10de" ] || continue
    case "$(cat "$d/device" 2>/dev/null)" in
        0x20c2|0x2082) ;;
        *) continue ;;
    esac
    found=1
    bdf="$(basename "$d")"
    f="$d/resource"
    if [ ! -r "$f" ]; then
        echo "$bdf: no resource file"
        fail=1
        continue
    fi
    s1="" e1="" s3="" e3=""
    read -r s1 e1 _ < <(sed -n '2p' "$f")
    read -r s3 e3 _ < <(sed -n '4p' "$f")
    if [ -z "${s1:-}" ] || [ -z "${e1:-}" ] \
       || [ -z "${s3:-}" ] || [ -z "${e3:-}" ]; then
        echo "$bdf: could not parse BAR1/BAR3"
        fail=1
        continue
    fi
    size1=$((e1 - s1 + 1))
    size3=$((e3 - s3 + 1))
    printf '%s: BAR1 %s-%s size=%#x  BAR3 %s-%s size=%#x\n' \
        "$bdf" "$s1" "$e1" "$size1" "$s3" "$e3" "$size3"
    if [ "$size1" -ne $((64 << 30)) ]; then
        echo "  unexpected BAR1 size (want 64 GiB)"
        fail=1
    fi
    if [ "$size3" -ne $((32 << 20)) ]; then
        echo "  unexpected BAR3 size (want 32 MiB)"
        fail=1
    fi
done
if [ "$found" -eq 0 ]; then
    echo "no CMP 170HX (10de:20c2/2082) found"
    fail=1
fi
echo

echo "== NVIDIA parameters =="
params="$(timeout 5s grep -E 'EnableResizableBar|RegistryDwords' \
    /proc/driver/nvidia/params 2>/dev/null)" || params=""
if [ -z "$params" ]; then
    echo "params read failed/timeout"
    fail=1
else
    printf '%s\n' "$params"
    case "$params" in
        *RMForceStaticBar1=1*) ;;
        *) echo "  missing RMForceStaticBar1=1"; fail=1 ;;
    esac
    case "$params" in
        *RMPcieP2PType=1*) ;;
        *) echo "  missing RMPcieP2PType=1"; fail=1 ;;
    esac
fi
echo

echo "== CUDA P2P transfer =="
if [ -f "$HERE/tools/p2p_test.py" ]; then
    timeout 300 python3 "$HERE/tools/p2p_test.py"
    rc=$?
    [ "$rc" -eq 0 ] || fail=1
else
    echo "tools/p2p_test.py not found"
    fail=1
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "VERIFY: PASS"
else
    echo "VERIFY: FAIL"
fi
exit "$fail"
