# cmpunlocker `--p2p`

BAR1-P2P comes from a third-party **cmpunlocker** build run with `--p2p` (not in
this repo; `--p2p` is a flag, not a separate project). This doc covers how the two
fit together and the validated install order.

| Piece | Responsibility |
|---|---|
| cmpunlocker (`--p2p`) | unlock the 64 GB framebuffer, build a P2P `nvidia.ko`, write the P2P dwords, create BAR1-P2P mappings |
| this shim | re-lay the PEX bridge windows and give each GPU a 64 GiB static BAR1 before `nvidia.ko` probes |

The driver sizes its static/BAR1-P2P mappings from BAR1 at probe, so it cannot use
a 64 GiB mapping when BAR1 is 64 MiB. The shim must therefore load before NVIDIA on
every boot - from the initramfs.

## Which cmpunlocker

A `--p2p`-capable build is required:

1. **Primary:** <https://github.com/bayley/cmpunlocker>
2. **Fallback**, when the above does not build against a newer NVIDIA driver: the
   maintained fork <https://github.com/sayhellotojungle/cmpunlocker>.

Either way the installed `nvidia.ko` must be a `--p2p` build with the P2P dwords.

## Tested invocation

```bash
git clone https://github.com/bayley/cmpunlocker
cd cmpunlocker
sudo ./install.sh --p2p --profile=8gb --no-passthrough --no-iommu
```

- `--p2p` - BAR1-P2P patches and the `RMForceStaticBar1=1` / `RMPcieP2PType=1` dwords.
- `--profile=8gb` - your memory-unlock profile.
- `--no-iommu` - used with `amd_iommu=on iommu=pt` on the kernel cmdline.
- `--no-passthrough` - fnOS-specific, not required by the shim.

Check your cmpunlocker version for the exact flags. The shim only needs a `--p2p`
`nvidia.ko` with the P2P dwords present.

## What the `--p2p` build writes

```ini
# /etc/modprobe.d/cmp-pcie-gen2.conf
options nvidia NVreg_RegistryDwords="RmForceEnableGen2=1;RMPcieLinkSpeed=0x1;RMForceStaticBar1=1;RMPcieP2PType=1"
```

It also regenerates the initramfs and, on `--p2p`, skips the live reload - so a cold
reboot is required. `RMForceStaticBar1=1` makes the driver keep an existing BAR1 as
static and use it for BAR1-P2P; with the shim BAR1 is already 64 GiB, so it logs
`keeping stock size` / `final size = 65536 MB` instead of resizing. `RMPcieP2PType=1`
enables the P2P path.

## Install and boot order

```bash
# 1. cmpunlocker --p2p
cd /path/to/cmpunlocker && sudo ./install.sh --p2p --profile=8gb --no-passthrough --no-iommu
# 2. this shim
cd /path/to/cmp-rebar-shim && make && sudo ./scripts/install.sh
# 3. cold reboot (power cycle), then:
sudo ./scripts/check_boot_logs.sh && sudo ./scripts/verify.sh
```

Install the shim after cmpunlocker because the latter rebuilds the initramfs. If you
did the shim first, re-run `sudo ./scripts/install.sh` (or `update-initramfs -u -k
$(uname -r)`) after it. What matters is the **boot order**: `cmp_rebar_shim` is listed
in `/etc/initramfs-tools/modules` and `nvidia` is not (udev loads NVIDIA after root).
Never list `nvidia` before the shim.

## The "old module" trap

cmpunlocker may run `nvidia-smi` early for detection, loading a non-P2P `nvidia.ko`
or the P2P module over a 64 MiB BAR1. Signs: `params` shows an old `RegistryDwords`
or `EnableResizableBar: 0`; `topo -p2p` = `GNS`; `canAccess=0`; GSP fails with
`Booter status 0x65`. Don't fix this by reloading modules in the same boot - install
the shim for the next boot and cold reboot, then check:

```bash
grep -E 'EnableResizableBar|RegistryDwords' /proc/driver/nvidia/params
# EnableResizableBar: 1
# RegistryDwords: "...;RMForceStaticBar1=1;RMPcieP2PType=1"
```

## If NVIDIA is already loaded

The shim must run before `nvidia.ko`. If NVIDIA is already up on a 64 MiB BAR1, don't
loop `nvidia-smi` (it can hang in D) - install the shim for the next boot and cold
reboot. For a one-off: unload NVIDIA (`nvidia_uvm nvidia_drm nvidia_modeset nvidia`),
`insmod ./cmp_rebar_shim.ko mode=1 commit=1`, confirm 64 GiB BARs in `dmesg`, then
`modprobe nvidia`. If the shim fails, do not load NVIDIA.

## Verify

`cmp_rebar_shim` lines appear before `NVRM`, ending in `final size = 65536 MB`, then
`python3 tools/p2p_test.py` shows `PASS` for all six ordered pairs
(`nvidia-smi topo -p2p r/w` = `OK`). If `nvidia-smi` has hung before, use the CUDA
test and `dmesg` instead.

## Kernel updates

Rebuild both for the new kernel - cmpunlocker first, then this shim
(`make clean && make && sudo ./scripts/install.sh`) - then cold reboot. Don't copy
old `.ko` files; vermagic must match.

Other failures and recovery: [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md).
