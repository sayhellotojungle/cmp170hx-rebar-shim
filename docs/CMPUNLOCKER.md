# Using this shim with cmpunlocker `--p2p`

The CMP 170HX BAR1-P2P support used in the tested setup comes from
**cmpunlocker** with its `--p2p` capability (third-party, not included in this
repository). `--p2p` is a mode/flag of cmpunlocker, not a separate project.
This document describes how the two pieces fit together and the install order
that was validated.

## What each piece does

| Piece | Responsibility |
|---|---|
| cmpunlocker (`--p2p`) | Unlocks the 64 GB framebuffer, builds a P2P-patched `nvidia.ko`, writes the P2P registry dwords, creates BAR1-P2P mappings |
| this shim | Re-lays-out the PEX bridge windows and gives each GPU a 64 GiB static BAR1 **before** `nvidia.ko` probes it |

The NVIDIA driver reads BAR1 at probe time and builds its static BAR1 / BAR1-P2P
mappings from that size. It cannot use a 64 GiB mapping if BAR1 is still
64 MiB. Therefore the shim must run **before** the NVIDIA driver on every boot;
the initramfs is the reliable place for that.

## Tested invocation

The tested cmpunlocker `--p2p` invocation was:

```bash
cd /path/to/cmpunlocker
sudo ./install.sh --p2p --profile=8gb --no-passthrough --no-gen2-service --no-iommu
```

- `--p2p` enables the BAR1-P2P patches and the `RMForceStaticBar1=1` /
  `RMPcieP2PType=1` registry dwords.
- `--no-iommu` was used together with `amd_iommu=on iommu=pt` on the kernel
  command line.
- `--profile=8gb` depends on your memory-unlock profile; use the value for your
  card.
- `--no-passthrough` / `--no-gen2-service` were used on the tested fnOS host.
  They are not required by the shim.

Use the flags documented by your cmpunlocker version. The shim only requires
that the installed `nvidia.ko` comes from a `--p2p` build and that the P2P
dwords are present.

## What cmpunlocker's `--p2p` build writes

Among other things, the `--p2p` build writes:

```ini
# /etc/modprobe.d/cmp-pcie-gen2.conf
options nvidia NVreg_RegistryDwords="RmForceEnableGen2=1;RMPcieLinkSpeed=0x1;RMForceStaticBar1=1;RMPcieP2PType=1"
```

and regenerates the initramfs. On `--p2p` builds it normally skips the live
NVIDIA module reload and requires a cold reboot.

`RMForceStaticBar1=1` tells the driver to treat the existing BAR1 as a static
window and to use it for the BAR1-P2P mapping. `RMPcieP2PType=1` enables the P2P
path. The driver's own resize logic is effectively bypassed: with the shim,
`pci_resource_len(BAR1)` is already 64 GiB, so the driver logs
`keeping stock size` / `final size = 65536 MB` instead of resizing.

## Recommended order

```text
1. cmpunlocker `--p2p` build/install      (BAR1 still 64 MiB; P2P not working yet)
2. this shim build/install                (adds shim to the final initramfs)
3. cold reboot                            (shim runs first, then NVIDIA)
4. verify                                 (BAR1 64 GiB + CUDA P2P test)
```

### Step by step

```bash
# 1. cmpunlocker with --p2p
cd /path/to/cmpunlocker
sudo ./install.sh --p2p --profile=8gb --no-passthrough --no-gen2-service --no-iommu

# 2. this shim
cd /path/to/cmp-rebar-shim
make
sudo ./scripts/install.sh

# 3. cold reboot (power cycle)

# 4. verify
sudo ./scripts/check_boot_logs.sh
sudo ./scripts/verify.sh
```

### Why not the other order?

The cmpunlocker installer regenerates the initramfs. If you install the shim
first, its initramfs changes can be overwritten by the later cmpunlocker run.
The shim's `/etc/initramfs-tools/modules` entry and modprobe file usually
survive, so the shim will still be included, but to avoid surprises re-run the
shim installer after cmpunlocker:

```bash
sudo ./scripts/install.sh
# or at minimum:
sudo update-initramfs -u -k $(uname -r)
```

The **boot order** is what actually matters: `cmp_rebar_shim` is listed in
`/etc/initramfs-tools/modules`; `nvidia` is not in the initramfs and is loaded by
udev after the root filesystem. Do not list `nvidia` before the shim.

## The "old module" trap

The cmpunlocker installer may invoke `nvidia-smi` early (for detection). That can
load the **old, non-P2P** `nvidia.ko` or the P2P module on top of a 64 MiB BAR1.
Symptoms:

- `/proc/driver/nvidia/params` shows the old `RegistryDwords` or
  `EnableResizableBar: 0`;
- `nvidia-smi topo -p2p` reports `GNS`;
- `cuDeviceCanAccessPeer()` returns 0;
- GSP initialization fails with `Booter status 0x65`.

Do not try to repair that inside the same boot by reloading modules repeatedly.
Make sure the shim is installed for the next boot, then **cold reboot**. After a
cold boot, check:

```bash
grep -E 'EnableResizableBar|RegistryDwords' /proc/driver/nvidia/params
```

Expected:

```text
EnableResizableBar: 1
RegistryDwords: "...;RMForceStaticBar1=1;RMPcieP2PType=1"
```

## If NVIDIA is already loaded

The shim must run before `nvidia.ko` probes the GPUs. If NVIDIA is already
loaded with the stock 64 MiB BAR1:

- do not run the P2P test or loop `nvidia-smi`; the driver may hang in D state;
- install the shim for the next boot and cold reboot, or
- only for a manual experiment: unload NVIDIA (`nvidia_uvm`, `nvidia_drm`,
  `nvidia_modeset`, `nvidia`), load the shim with `mode=1 commit=1`, confirm
  64 GiB BARs in `dmesg`, then `modprobe nvidia`. If the shim fails, do not load
  NVIDIA.

## Verification of the combined stack

Log order on a healthy cold boot:

```text
cmp_rebar_shim: apply commit=1 ...
cmp_rebar_shim: 0000:05:00.0: REBAR cap locked (0x400), unlocking
cmp_rebar_shim: verify ... BAR1 ... len=0x1000000000
...
NVRM: CMP BAR1: final size = 65536 MB
```

Then:

```bash
python3 tools/p2p_test.py
```

All six ordered pairs must report `PASS`. `nvidia-smi topo -p2p r/w` should
report `OK` for every pair. (If `nvidia-smi` has hung before, use the CUDA test
and `dmesg` first; do not loop `nvidia-smi`.)

## Kernel updates

The NVIDIA module and this shim are both kernel-specific. After a kernel update:

1. rebuild/reinstall cmpunlocker with `--p2p` for the new kernel;
2. rebuild/reinstall this shim (`make clean && make && sudo ./scripts/install.sh`);
3. cold reboot.

Do not copy the old `.ko` files; vermagic must match.

## Common combined-stack failures

| Symptom | Likely cause |
|---|---|
| `nvidia-smi` hangs / D state | NVIDIA loaded with 64 MiB BAR1; GSP failed |
| `topo -p2p` = GNS, `canAccess=0` | NVIDIA loaded before the shim; old BAR1 |
| `RmInitAdapter failed`, `Booter status 0x65` | BAR1 not 64 GiB at probe |
| P2P works after manual reload but not after reboot | shim missing from initramfs, or NVIDIA in initramfs before it |
| `request_irq() failed (-4)` | cascade from a failed GPU init; cold reboot |
| `CMP BAR1: keeping stock size` | benign with `RMForceStaticBar1=1`; check the `final size` line |

See [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md) for details.
