#!/usr/bin/env bash
# Read-only boot diagnostics. Does not call nvidia-smi.
set -u

echo "== kernel / uptime =="
uname -r
uptime
echo
echo "== modules =="
lsmod | grep -iE 'nvidia|cmp_rebar' || echo none
echo
echo "== D-state processes =="
ps -eo pid,ppid,stat,wchan:32,comm | awk '$3 ~ /D/'
echo
echo "== shim dmesg =="
dmesg | grep -iE 'cmp_rebar_shim' | tail -60
echo
echo "== NVRM critical =="
dmesg | grep -iE 'NVRM.*(RmInitAdapter|CMP BAR1|Xid|fallen off|normal BooterLoad status)' | tail -40
echo
echo "== CMP BARs (auto-detected by PCI ID) =="
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
        continue
    fi
    s1="" e1="" s3="" e3=""
    read -r s1 e1 _ < <(sed -n '2p' "$f")
    read -r s3 e3 _ < <(sed -n '4p' "$f")
    if [ -z "${s1:-}" ] || [ -z "${e1:-}" ] \
       || [ -z "${s3:-}" ] || [ -z "${e3:-}" ]; then
        echo "$bdf: could not parse BAR1/BAR3"
        continue
    fi
    printf '%s: BAR1 %s-%s size=%#x  BAR3 %s-%s size=%#x\n' \
        "$bdf" "$s1" "$e1" "$((e1 - s1 + 1))" \
        "$s3" "$e3" "$((e3 - s3 + 1))"
done
[ "$found" -eq 0 ] && echo "no CMP 170HX (10de:20c2/2082) found"
echo
echo "== nvidia params (timeout 5s) =="
timeout 5s grep -E 'EnableResizableBar|RegistryDwords' \
    /proc/driver/nvidia/params 2>/dev/null \
    || echo "params read failed/timeout"
echo
echo "== journal warnings =="
if command -v journalctl >/dev/null 2>&1; then
    timeout 20s journalctl -b -p warning --no-pager | tail -40
else
    echo "journalctl not available"
fi
