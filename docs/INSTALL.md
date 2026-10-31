# Installation

This document covers the shim itself. For the required install order with the
cmpunlocker with `--p2p`, read [`CMPUNLOCKER.md`](CMPUNLOCKER.md) first.

## Requirements

- A recent Linux kernel (tested on `6.18.18` fnOS; build-checked against 6.12
  headers). Probe mode logs `pci_host_bridge.preserve_config` / `size_windows`
  (host-bridge fields present since 6.12). On kernels older than 6.12 that one
  `pr_info()` is compiled out through a `LINUX_VERSION_CODE` guard, so the
  module still builds there; the apply path never used those fields.
- Linux kernel headers for the **running** kernel
  (`/lib/modules/$(uname -r)/build`)
- `make`, a C compiler compatible with your kernel, `depmod`
- `initramfs-tools` (`update-initramfs`, `lsinitramfs`) for the tested setup
  (Debian/Ubuntu/fnOS). See [dracut](#dracut) for other distributions.
- root access
- A recovery path: an older kernel in GRUB, a live USB, or physical access

The shim is built against the running kernel. Rebuild it after every kernel
update.

## Build

```bash
cd cmp-rebar-shim
make
```

The output is `cmp_rebar_shim.ko`. To build against a different kernel tree:

```bash
make KDIR=/lib/modules/<version>/build
```

## Dry-run first

`mode=0` is a read-only probe: it logs the CMP devices, BARs, ReBAR
capabilities and the current bridge-window chain, and changes nothing:

```bash
sudo insmod ./cmp_rebar_shim.ko mode=0
dmesg | tail -80
sudo rmmod cmp_rebar_shim
```

`mode=1` without `commit=1` prints the plan and performs an in-memory allocation
test. It does **not** write bridge windows, BARs, or PCFG registers.

```bash
sudo insmod ./cmp_rebar_shim.ko mode=1
dmesg | tail -80
sudo rmmod cmp_rebar_shim
```

`mode=2` runs only the resource test:

```bash
sudo insmod ./cmp_rebar_shim.ko mode=2
dmesg | tail -40
sudo rmmod cmp_rebar_shim
```

Check the plan before committing:

- `root window` is the root prefetchable window of the CMP switch;
- `top needs` should be 384 GiB on the tested tree;
- `top allocation test: ret=0` — the fixed top window
  (`0x1000000000-0x6fffffffff` on the tested machine, i.e. below the iGPU
  aperture) is free inside the root window;
- the per-GPU `-> 64G/32M` lines.

If the plan is wrong, do not install. See
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md).

## Install for the next boot

```bash
sudo ./scripts/install.sh
```

The script writes:

| Path | Content |
|---|---|
| `/lib/modules/$(uname -r)/extra/cmp_rebar_shim.ko` | module |
| `/etc/modprobe.d/cmp-rebar-shim.conf` | `options cmp_rebar_shim mode=1 commit=1` |
| `/etc/initramfs-tools/modules` | `cmp_rebar_shim` |
| initramfs | rebuilt with `update-initramfs -u -k $(uname -r)` |

It does **not** load the module now. It is loaded from the initramfs on the next
boot, before the root filesystem and before udev loads NVIDIA. It refuses to
proceed if `nvidia` is listed in the initramfs before `cmp_rebar_shim`.

`--dry-run` prints the actions without changing anything:

```bash
./scripts/install.sh --dry-run
```

### Manual install

```bash
KVER=$(uname -r)
sudo install -D -m 0644 cmp_rebar_shim.ko \
    /lib/modules/$KVER/extra/cmp_rebar_shim.ko
printf 'options cmp_rebar_shim mode=1 commit=1\n' \
    | sudo tee /etc/modprobe.d/cmp-rebar-shim.conf
grep -qxF cmp_rebar_shim /etc/initramfs-tools/modules \
    || printf '\ncmp_rebar_shim\n' \
       | sudo tee -a /etc/initramfs-tools/modules
sudo depmod -a "$KVER"
sudo update-initramfs -u -k "$KVER"
lsinitramfs /boot/initrd.img-$KVER | grep cmp_rebar_shim.ko
```

## Cold reboot

A **cold reboot** (power cycle) is required, not just a module reload:

- the shim must run before the NVIDIA driver probes the GPUs;
- the cmpunlocker `--p2p` build normally requires a cold reboot anyway;
- a warm reboot may leave the GPU/PCFG state from the previous boot.

After the reboot the shim's `dmesg` lines must appear **before** the `NVRM`
lines. Timing example from the tested machine (yours will differ):

```text
~5s   cmp_rebar_shim: apply commit=1 ...
~5s   cmp_rebar_shim: REBAR cap locked (0x400), unlocking
~5s   cmp_rebar_shim: verify ... BAR1 ... len=0x1000000000
~14s  NVRM: CMP BAR1: final size = 65536 MB
```

## Verify

```bash
sudo ./scripts/check_boot_logs.sh   # log-only, no nvidia-smi
sudo ./scripts/verify.sh            # BAR checks + real CUDA P2P transfer
```

See [`VERIFY.md`](VERIFY.md).

## Install order with cmpunlocker `--p2p`

The validated order is:

1. cmpunlocker with `--p2p` (`--p2p --no-iommu ...`)
2. this shim (`sudo ./scripts/install.sh`)
3. cold reboot

If you installed the shim first, re-run `sudo ./scripts/install.sh` after
cmpunlocker so the final initramfs contains the shim. The full details, flags,
and failure modes are in [`CMPUNLOCKER.md`](CMPUNLOCKER.md).

## Kernel updates

For every new kernel:

```bash
make clean
make
sudo ./scripts/install.sh
```

If you also use cmpunlocker's `--p2p` mode, rebuild/reinstall it for the new
kernel first, then re-run the shim installer, then cold reboot. Do not copy an
old `.ko` to a new kernel; the vermagic must match.

## dracut

The tested setup uses `initramfs-tools`. On a dracut system the shim must still
be in the initramfs **before** NVIDIA. One way:

```bash
sudo install -D -m 0644 cmp_rebar_shim.ko \
    /lib/modules/$(uname -r)/extra/cmp_rebar_shim.ko
sudo depmod -a
printf 'force_drivers+=" cmp_rebar_shim "\n' \
    | sudo tee /etc/dracut.conf.d/90-cmp-rebar-shim.conf
sudo dracut -f
```

Do not rely on udev to load the shim after NVIDIA. Verify with
`lsinitrd /boot/initramfs-$(uname -r).img | grep cmp_rebar_shim`. This path is
not tested by this project; adjust for your distribution.

## Uninstall

```bash
sudo ./scripts/uninstall.sh
```

This removes the modprobe file, the initramfs module entry, the installed `.ko`,
and regenerates the initramfs. The current BAR layout stays until the next
reboot; reboot to return to the firmware layout.

Manual removal:

```bash
sudo rm -f /etc/modprobe.d/cmp-rebar-shim.conf
sudo sed -i '/^[[:space:]]*cmp_rebar_shim[[:space:]]*$/d' \
    /etc/initramfs-tools/modules
sudo rm -f /lib/modules/$(uname -r)/extra/cmp_rebar_shim.ko
sudo depmod -a
sudo update-initramfs -u -k $(uname -r)
```

## Recovery

If the machine hangs after an install:

1. Power-cycle and select an **older kernel** in GRUB (the previously working
   entry; its initramfs does not contain the shim).
2. Or boot with `modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset` added to
   the kernel command line. This prevents udev from auto-loading NVIDIA so you
   can inspect the shim log. Note that `nvidia-modprobe` may bypass blacklists
   on some distributions; the older-kernel path is the most reliable.
3. Or boot a live USB, mount the root filesystem, and remove the shim from
   `/etc/initramfs-tools/modules` and `/etc/modprobe.d/cmp-rebar-shim.conf`,
   then run `update-initramfs` in a chroot.
4. Do not repeatedly run `nvidia-smi` if it hangs; unload NVIDIA or cold reboot.

## Files written by the installer

```text
/lib/modules/<kver>/extra/cmp_rebar_shim.ko
/etc/modprobe.d/cmp-rebar-shim.conf
/etc/initramfs-tools/modules        (one line: cmp_rebar_shim)
/boot/initrd.img-<kver>             (regenerated)
```

The shim does not modify the kernel image, `/boot/vmlinuz-*`, distro prebuilt
modules, or the NVIDIA driver.
