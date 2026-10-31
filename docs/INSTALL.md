# Install

For the required order with cmpunlocker `--p2p`, read
[`CMPUNLOCKER.md`](CMPUNLOCKER.md) first.

## Requirements

- A recent x86_64 kernel (tested on fnOS `6.18.18`; the probe log reads two
  host-bridge fields that only exist on 6.12+, guarded otherwise).
- Kernel headers for the **running** kernel (`/lib/modules/$(uname -r)/build`),
  `make`, a C compiler, `depmod`.
- `initramfs-tools` for the tested path; dracut below for other distros.
- root, and a recovery path (older GRUB kernel or live USB).

Rebuild the module after every kernel update.

## Build

```bash
make                       # builds cmp_rebar_shim.ko for the running kernel
make KDIR=/lib/modules/<ver>/build   # or a specific tree
```

## Dry-run first (writes nothing)

```bash
sudo insmod ./cmp_rebar_shim.ko mode=0   # probe: GPUs, BARs, ReBAR caps, bridge windows
sudo insmod ./cmp_rebar_shim.ko mode=1   # dry-run: per-bridge plan + root-window fit check
sudo insmod ./cmp_rebar_shim.ko mode=2   # resource test: does a 384 GiB window fit
dmesg | tail -80
sudo rmmod cmp_rebar_shim
```

Check `root window`, `top needs` (384 GiB on the tested tree),
`top allocation test: ret=0`, and the per-GPU `-> 64G/32M` lines. If wrong, do not
install ([`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)).

## Install for the next boot

```bash
sudo ./scripts/install.sh          # add --dry-run to preview
```

It writes:

| Path | Content |
|---|---|
| `/lib/modules/$(uname -r)/extra/cmp_rebar_shim.ko` | module |
| `/etc/modprobe.d/cmp-rebar-shim.conf` | `options cmp_rebar_shim mode=1 commit=1` |
| `/etc/initramfs-tools/modules` | one line `cmp_rebar_shim` |
| initramfs | rebuilt with `update-initramfs -u -k $(uname -r)` |

It does not load the module now - the initramfs loads it before NVIDIA on the next
boot. It refuses if `nvidia` is listed before the shim. Manual equivalent:

```bash
KVER=$(uname -r)
sudo install -D -m 0644 cmp_rebar_shim.ko /lib/modules/$KVER/extra/cmp_rebar_shim.ko
printf 'options cmp_rebar_shim mode=1 commit=1\n' | sudo tee /etc/modprobe.d/cmp-rebar-shim.conf
grep -qxF cmp_rebar_shim /etc/initramfs-tools/modules || printf '\ncmp_rebar_shim\n' | sudo tee -a /etc/initramfs-tools/modules
sudo depmod -a "$KVER" && sudo update-initramfs -u -k "$KVER"
lsinitramfs /boot/initrd.img-$KVER | grep cmp_rebar_shim.ko
```

## dracut

`initramfs-tools` is what was tested. On dracut the shim must still be in the
initramfs before NVIDIA (not verified here):

```bash
sudo install -D -m 0644 cmp_rebar_shim.ko /lib/modules/$(uname -r)/extra/cmp_rebar_shim.ko
sudo depmod -a
printf 'force_drivers+=" cmp_rebar_shim "\n' | sudo tee /etc/dracut.conf.d/90-cmp-rebar-shim.conf
sudo dracut -f
```

## Cold reboot, then verify

A power cycle (not a warm reboot) is required so the shim runs before NVIDIA. The
shim's `dmesg` lines must precede the `NVRM` lines. Then:

```bash
sudo ./scripts/check_boot_logs.sh   # log-only
sudo ./scripts/verify.sh            # BARs + real CUDA P2P
```

## Kernel updates

For each new kernel rebuild and reinstall, cmpunlocker first if you use `--p2p`:

```bash
make clean && make && sudo ./scripts/install.sh
```

Do not copy an old `.ko` - vermagic must match.

## Uninstall

```bash
sudo ./scripts/uninstall.sh
```

Removes the modprobe file, the initramfs entry and the `.ko`, and regenerates the
initramfs. The current BAR layout stays until reboot.

## Recovery

If the box hangs after an install: boot an older kernel from GRUB (its initramfs
has no shim); or boot a live USB, remove the shim from `/etc/initramfs-tools/modules`
and `/etc/modprobe.d/cmp-rebar-shim.conf`, and re-run `update-initramfs` in a chroot.
`nvidia-modprobe` may bypass `modprobe.blacklist=nvidia`, so the older-kernel path is
the reliable one.
