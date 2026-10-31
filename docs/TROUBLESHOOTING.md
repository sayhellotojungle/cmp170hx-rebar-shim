# Troubleshooting

Start with `sudo ./scripts/check_boot_logs.sh` (read-only) and collect `dmesg`,
`/proc/driver/nvidia/params` and the BAR sizes. Don't loop `nvidia-smi` if the
driver is stuck.

| Symptom | Likely cause | Fix |
|---|---|---|
| `cmp_rebar_shim` not in `lsmod` / no log | not loaded from initramfs | `lsinitramfs`, check GRUB `initrd` path, rebuild, cold reboot |
| `REBAR cap locked (0x400), unlocking` | normal cold boot | none |
| `REBAR cap unreadable (0xffffffff)` | BAR0/PCFG unmapped, GPU off D0 | cold reboot; check D0, BAR0, bridge windows |
| `unlock did not take (0x..)` | not in D0 / PCFG inaccessible / wrong offsets | cold reboot, dry-run; verify `CMP_PCFG_*` |
| `top allocation test: ret != 0` | root window occupied / alignment | inspect `/proc/iomem`, adjust `CMP_TOP_START`/sizes |
| `unexpected top size` | topology differs | dry-run, adapt sizes/alignment |
| `RmInitAdapter failed`, `Booter status 0x65/0xffff` | BAR1 not 64 GiB at probe | make the shim run before NVIDIA; cold reboot |
| `nvidia-smi` hangs / D state | GSP failed on 64 MiB BAR1 | stop the caller, `rmmod nvidia*` if possible, else cold reboot; don't loop |
| `topo -p2p` = GNS, `canAccess=0` | NVIDIA loaded before the shim | cold reboot with the shim in the initramfs |
| `CMP BAR1: keeping stock size` | benign with `RMForceStaticBar1=1` | check the `final size = 65536 MB` line |
| `modprobe: ... not found` / `version magic` mismatch | module not in initramfs / wrong kernel | `depmod` + `update-initramfs`; `make clean && make` |

## Shim not loaded

```bash
lsmod | grep cmp_rebar
dmesg | grep -i cmp_rebar
lsinitramfs /boot/initrd.img-$(uname -r) | grep cmp_rebar
grep -n cmp_rebar /etc/initramfs-tools/modules
```

If the `.ko` is in the initramfs but there is no log, it failed to insert - look for
`Unknown symbol` / `version magic` in the log. If it is missing: `sudo depmod -a` and
`sudo update-initramfs -u -k $(uname -r)`. Make sure `cmp_rebar_shim` is listed before
any `nvidia` (or that NVIDIA is not in the initramfs at all).

## Top allocation fails

```text
cmp_rebar_shim: fixed top window 0x1000000000-0x6fffffffff unavailable in root window
```

The root prefetchable window does not contain that 384 GiB range, so the module fails
before touching hardware. Check `cat /proc/iomem` for the root window and any occupant
(tested: iGPU at `0x7fd0000000-0x7fe01fffff`). Adjust `CMP_TOP_START` in
`cmp_rebar_shim.c`, the sizes, or free the range.

## NVIDIA GSP failure / D state

`Booter status 0x65`, `RmInitAdapter failed!`, or `nvidia-smi` stuck in `D` means the
driver probed a 64 MiB BAR1. Confirm order (`cmp_rebar_shim` before `NVRM`), 64 GiB
BARs in the log, and the P2P dwords in `params`. Fix the shim/initramfs first, then
cold reboot - a failed GSP init can leave the GPU in a bad state. Recovery: stop the
media/persistence service, `sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia`
if it unloads, otherwise power-cycle.

## Recovery

Boot an older kernel from GRUB (its initramfs has no shim), or a live USB to remove
the shim from `/etc/initramfs-tools/modules` and `/etc/modprobe.d/cmp-rebar-shim.conf`
and re-run `update-initramfs` in a chroot. Then `sudo ./scripts/uninstall.sh` to remove
it permanently.
