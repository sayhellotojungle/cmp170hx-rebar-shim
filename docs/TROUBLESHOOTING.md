# Troubleshooting

Always start with the log-only check:

```bash
sudo ./scripts/check_boot_logs.sh
```

Collect `dmesg`, `/proc/driver/nvidia/params`, and the BAR sizes. Avoid repeated
`nvidia-smi` calls if the driver is stuck.

## Symptom table

| Symptom | Likely cause | Fix |
|---|---|---|
| `cmp_rebar_shim` not in `lsmod` | module not loaded from initramfs | check initramfs contents/entry, rebuild, cold reboot |
| No `cmp_rebar_shim` log at all | initramfs not used, or module failed to insert | `lsinitramfs`, `dmesg`, bootloader initrd path |
| `REBAR cap locked (0x400), unlocking` | normal cold boot, shim unlocks | none |
| `unlock did not take (0x...)` | GPU not in D0 / PCFG inaccessible / wrong offsets | cold reboot, check power, verify offsets |
| `REBAR cap unreadable (0xffffffff)` | BAR0/PCFG not mapped, GPU powered off | check D0, BAR0, bridge windows |
| `top allocation test: ret != 0` | root window occupied / alignment | inspect `/proc/iomem`, adjust plan |
| `unexpected top size` | topology differs from tested tree | dry-run, adapt sizes |
| `RmInitAdapter failed`, `Booter status 0x65/0xffff` | BAR1 not 64 GiB at probe | make shim run before NVIDIA; cold reboot |
| `nvidia-smi` hangs / D state | GSP failed with 64 MiB BAR1 | unload NVIDIA or cold reboot; do not loop |
| `topo -p2p` = GNS, `canAccess=0` | NVIDIA loaded before the shim / old module | cold reboot with shim in initramfs |
| P2P `DEST MISMATCH` | test reuses overwritten buffers | use this repo's test (reinitializes per pair) |
| `request_irq() failed (-4)` | cascade after failed GPU init | cold reboot after fixing BAR1 |
| `CMP BAR1: keeping stock size` | benign with `RMForceStaticBar1=1` | check `final size = 65536 MB` |
| `modprobe: FATAL: Module ... not found` | module not in initramfs | `depmod`, `update-initramfs` |
| `version magic ... should be` | module built for a different kernel | `make clean && make`, reinstall |
| No BAR1 change after reboot | shim not in the initrd the bootloader uses | check GRUB/`initrd` path |
| `nvidia-smi` reports 64 MiB BAR1 | shim did not run | `dmesg`, initramfs, cold reboot |

## Shim not loaded

Check:

```bash
lsmod | grep cmp_rebar
dmesg | grep -i cmp_rebar
lsinitramfs /boot/initrd.img-$(uname -r) | grep cmp_rebar
grep -n cmp_rebar /etc/initramfs-tools/modules
cat /etc/modprobe.d/cmp-rebar-shim.conf
```

If the `.ko` is in the initramfs but there is no dmesg line, the module may have
failed to insert. Check the initramfs's `modules.dep` and the kernel log for
`Unknown symbol` / `version magic`.

If the `.ko` is missing from the initramfs:

```bash
sudo depmod -a
sudo update-initramfs -u -k $(uname -r)
```

If you use a bootloader that points at a different initrd (not
`/boot/initrd.img-$(uname -r)`), regenerate or configure that initrd.

If your distro puts `nvidia.ko` in the initramfs, make sure `cmp_rebar_shim` is
listed **before** it, or remove NVIDIA from the initramfs. On the tested fnOS
setup NVIDIA is not in the initramfs.

## ReBAR unlock errors

### `REBAR cap locked (0x400), unlocking`

This is normal on a cold boot. The shim writes the unlock sequence and then
prints `REBAR ctrl now 0x1001 (size=16)`.

### `unlock did not take (0x...)`

The write sequence did not set the capability bit. Possible causes:

- the GPU is not powered / not in D0;
- the PCFG offset is wrong for this GPU;
- another driver is holding/resetting the GPU.

Try a cold reboot and the dry-run first. If it persists, verify the
`CMP_PCFG_OFFSET` / register offsets for your card.

### `REBAR cap unreadable (0xffffffff)`

BAR0/PCFG MMIO is not accessible. Check that the GPU is in D0 and that BAR0 is
enabled. The shim calls `pci_set_power_state(PCI_D0)` and waits 20 ms before
touching BAR0; if the platform still cannot wake the GPU at initramfs time, the
module may need to run later (for example via a udev/systemd hook) or the
platform power settings need fixing.

## Top allocation failure

```text
cmp_rebar_shim: top allocation test: ret=-...
cmp_rebar_shim: fixed top window 0x1000000000-0x6fffffffff unavailable in root window (ret=-...)
```

The root prefetchable window does not contain the fixed top window
`0x1000000000-0x6fffffffff` (384 GiB at 64 GiB alignment). The module fails
before touching any GPU or bridge register. Inspect:

```bash
cat /proc/iomem
```

Look for the root prefetchable window and any device occupying the range. On the
tested machine the iGPU is at `0x7fd0000000-0x7fe01fffff`, and the shim uses
`0x1000000000-0x6fffffffff`. The top window start is `CMP_TOP_START` in
`cmp_rebar_shim.c`; if your firmware layout differs you may need to adapt it,
the sizes, or free the range.

## NVIDIA GSP failure

```text
NVRM: GPU0 s_executeBooterUcode_TU102: failed to execute Booter: status 0x65
NVRM: GPU0 kgspExecuteBooterLoad_TU102: failed to execute Booter Load: 0x65
NVRM: GPU0 RmInitAdapter: Cannot initialize GSP firmware RM
NVRM: GPU 0000:05:00.0: RmInitAdapter failed!
```

This is the signature of the NVIDIA driver probing with a **64 MiB BAR1** while
the `--p2p` build expects a 64 GiB static BAR1. Check the boot order:

1. Is the shim loaded from the initramfs before NVIDIA?
2. Does the shim log show 64 GiB BARs?
3. Does `/proc/driver/nvidia/params` show the P2P dwords?

If the shim failed, fix it first. If the shim succeeded but NVIDIA still fails,
cold reboot; a failed GSP initialization can leave the GPU in a bad state.

Do not loop `nvidia-smi`; it can block in D state. Unload the NVIDIA modules or
power-cycle.

## `nvidia-smi` hangs / D state

Symptoms:

```text
ps -eo pid,stat,wchan:32,comm | awk '$2 ~ /D/'
```

shows `nvidia-smi`/`nvidia-persistenced`/a media service in `D`.

This happened in the tested environment when NVIDIA tried to initialize GSP on a
64 MiB BAR1. Recovery:

1. Stop the service that calls it (media services, persistence daemon).
2. `sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia` if possible.
3. If unload hangs, cold reboot.
4. Fix the shim/initramfs so BAR1 is 64 GiB before NVIDIA loads.

## P2P says GNS / canAccess = 0

```bash
nvidia-smi topo -p2p r
```

reports `GNS`, or:

```python
cuDeviceCanAccessPeer(...) == 0
```

The NVIDIA driver was loaded with the wrong BAR1 size or without the P2P
registry dwords. Check:

- `dmesg` order: all `cmp_rebar_shim` lines appear **before** any `NVRM` line
  (the exact timings vary with your machine);
- `grep -E 'EnableResizableBar|RegistryDwords' /proc/driver/nvidia/params`;
- `RMPcieP2PType=1` and `RMForceStaticBar1=1` are present;
- the running `nvidia.ko` is the `--p2p` build (not an older module loaded by
  the installer's early `nvidia-smi`).

The fix is usually a cold reboot with the shim in the initramfs.

## P2P test `DEST MISMATCH`

If you write your own test, remember that a copy overwrites the destination
buffer. If you reuse the same buffers for the next pair and use the overwritten
buffer as a source, the patterns will not match. This repository's
`tools/p2p_test.py` reinitializes both source and destination patterns before
every pair.

## `request_irq() failed (-4)`

Often a cascade after a failed GPU initialization. Fix the BAR1/GSP problem and
cold reboot; the IRQ allocation usually recovers.

## `CMP BAR1: keeping stock size`

```text
NVRM: CMP BAR1: before CYA=0x00000030 CFG=0x8000000a CAP=0x001ffc00
NVRM: CMP BAR1: keeping stock size (dynamic mapping); set NVreg_EnableResizableBar=1 to resize
NVRM: CMP BAR1: final size = 65536 MB
```

With `RMForceStaticBar1=1`, the driver intentionally keeps the existing static
window. The message suggests setting `EnableResizableBar=1`, but even with it set
the driver sees `old_size == requested_size` and skips its own resize. The
`final size = 65536 MB` line is the one to trust.

## `unexpected top size`

```text
cmp_rebar_shim: unexpected top size 0x... (want 384G)
```

Your topology needs a different top window. Run the dry-run, inspect
`dump_plan_walk()` output, and adapt the sizes/alignment. Do not commit blindly.

## Kernel update / vermagic mismatch

```text
version magic '...' should be '...'
```

The module was built for a different kernel. Rebuild:

```bash
make clean
make
sudo ./scripts/install.sh
```

If you use cmpunlocker with `--p2p`, rebuild its P2P driver for the new kernel first.

## Recovery

- GRUB: boot the previous kernel (its initramfs does not contain the shim).
- Kernel command line: add `modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset`
  to keep NVIDIA from auto-loading so you can inspect the shim. Some setups'
  `nvidia-modprobe` may bypass this; the older-kernel path is more reliable.
- Live USB: mount the root filesystem and remove the shim from
  `/etc/initramfs-tools/modules` and `/etc/modprobe.d/cmp-rebar-shim.conf`, then
  run `update-initramfs` in a chroot.
- After any recovery, run `sudo ./scripts/uninstall.sh` if you want to remove
  the shim permanently.
