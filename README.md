# cmp-rebar-shim

Out-of-tree Linux kernel module that re-lays the PCIe prefetchable windows in
front of NVIDIA CMP 170HX GPUs and gives each card a **64 GiB BAR1 + 32 MiB
BAR3 before `nvidia.ko` probes** - one module in the initramfs, no kernel
patch and no custom kernel image.

Experimental; it rewrites PCI bridge windows and vendor GPU registers - see
[Safety and limitations](#safety-and-limitations). AI-assisted, validated on real hardware
([`docs/TESTED-CONFIG.md`](docs/TESTED-CONFIG.md)). Addresses, bus numbers,
sizes and flags in this repo are from one tested machine - examples, not
requirements. Not affiliated with NVIDIA. GPL-2.0.

## Problem

GPUs behind a PCIe switch (tested: PEX88096) are enumerated with a 64 MiB
BAR1. Resizing it means resizing every parent bridge window, and the stock
kernel cannot: bridge-window sizing ignores the 64 GiB BAR alignment, and the
root prefetchable window is never recomputed. The resize fails, so a driver
that *requires* the big BAR hangs at probe:

| Symptom | Cause |
|---|---|
| `NVRM: ... failed to execute Booter: status 0x65`, `RmInitAdapter failed!`, `nvidia-smi` stuck in `D` | [cmpunlocker](docs/CMPUNLOCKER.md) `--p2p` driver probed with a 64 MiB BAR1; GSP init failed |
| `cuDeviceCanAccessPeer()` = 0, `nvidia-smi topo -p2p` = `GNS` | driver kept the 64 MiB dynamic BAR1; P2P off |

A kernel patch would fix this at the source, but it means maintaining a custom
kernel per update - impossible on trimmed, prebuilt appliance kernels (fnOS /
飞牛OS, where this project started).

The shim reaches the same end state from outside: one module loaded from the
initramfs before the driver probes, using only the stock kernel's exported PCI
resource APIs. Afterwards each GPU has BAR1 = 64 GiB / BAR3 = 32 MiB, the
driver logs `NVRM: CMP BAR1: final size = 65536 MB`, and P2P is verified with
real 64 MiB `cuMemcpyPeer` transfers ([`tools/p2p_test.py`](tools/p2p_test.py)).

The shim does **not**: unlock the framebuffer, implement BAR1-P2P (both
cmpunlocker's job), or generalize - the sizes and PCFG offsets are for the
CMP 170HX and this switch tree.

## Install (with cmpunlocker `--p2p`)

Install the shim **after** cmpunlocker (its installer rebuilds the initramfs).
At boot the shim must load before NVIDIA; the installer refuses if `nvidia` is
listed before it in `/etc/initramfs-tools/modules`.

```bash
# 1. cmpunlocker --p2p first (which repo, flag meanings: docs/CMPUNLOCKER.md)
cd /path/to/cmpunlocker
sudo ./install.sh --p2p --profile=8gb --no-passthrough --no-iommu

# 2. this shim
cd cmp-rebar-shim
make
sudo ./scripts/install.sh

# 3. cold reboot (power cycle, not a warm reboot)
# 4. verify
sudo ./scripts/verify.sh
```

Step 2 copies the `.ko` to `/lib/modules/$(uname -r)/extra/`, writes
`options cmp_rebar_shim mode=1 commit=1` to `/etc/modprobe.d/`, appends the
module to `/etc/initramfs-tools/modules`, and runs `depmod` + `update-initramfs`.
Manual and dracut equivalents: [`docs/INSTALL.md`](docs/INSTALL.md).

If you installed the shim before cmpunlocker, re-run `sudo ./scripts/install.sh`
after it. If NVIDIA is already loaded with a 64 MiB BAR1, load the shim on the
*next* boot via the initramfs, not on top of the running driver.

## Try it first (no hardware writes)

```bash
make

sudo insmod ./cmp_rebar_shim.ko mode=0   # probe: GPUs, BARs, ReBAR caps, bridge windows
sudo insmod ./cmp_rebar_shim.ko mode=2   # resource test: does a 384 GiB window fit (in-memory)
sudo insmod ./cmp_rebar_shim.ko mode=1   # dry-run: per-bridge plan + fits-root-window check
dmesg | tail -80
sudo rmmod cmp_rebar_shim
```

In the dry-run, check `root window`, `top needs` (384 GiB for the tested 3-card
tree), `top allocation test: ret=0`, and the per-GPU `-> 64G/32M` lines. If the
plan is wrong for your tree, do not install
([`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)).

Manual live commit, only when NVIDIA is unloaded:

```bash
sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia 2>/dev/null || true
sudo insmod ./cmp_rebar_shim.ko mode=1 commit=1   # expect "REBAR ctrl now ..." + 64G BARs
sudo modprobe nvidia && sudo modprobe nvidia-uvm
```

If the shim fails, do not load NVIDIA - cold reboot.

## Verify

```bash
sudo ./scripts/check_boot_logs.sh   # log-only, no nvidia-smi
python3 tools/p2p_test.py           # real CUDA P2P copies
```

Healthy cold boot: `cmp_rebar_shim` lines appear **before** `NVRM`, then
`NVRM: CMP BAR1: final size = 65536 MB`. Your addresses differ; the **sizes**
and that order are what matter. `CMP BAR1: keeping stock size` is benign with
`RMForceStaticBar1=1` - the `final size` line is authoritative. Full expected
output: [`docs/VERIFY.md`](docs/VERIFY.md).

## How it works

On `mode=1 commit=1`, before root is mounted and NVIDIA probes, the shim:

1. finds the CMP GPUs (`10de:20c2` / `10de:2082`) and walks up to the top
   switch bridge;
2. computes each bridge's prefetchable window (64 GiB + 32 MiB per GPU) and
   checks the fixed 384 GiB top window (starting at 64 GiB) fits the root
   prefetchable window;
3. per GPU: forces D0, waits for PCFG, unlocks ReBAR via BAR0 MMIO
   (`CYA=0x30`, `BAR1_CONFIG=0x8000000a`, ReBAR size 16), resizes BAR1/BAR3 to
   64 GiB / 32 MiB;
4. releases the old child windows, sets the new bridge sizes, inserts the top
   window, runs `pci_bus_assign_resources()`, programs the bridge registers and
   re-enables memory decode.

It only touches prefetchable windows and CMP devices, and does not modify the
kernel, distro modules, or the NVIDIA driver. Details:
[`docs/HOW-IT-WORKS.md`](docs/HOW-IT-WORKS.md).

## Layout (tested tree)

Root prefetchable window `0xe40000000-0x7fffffffff`; the shim uses the top
window `0x1000000000-0x6fffffffff` (384 GiB), below the iGPU aperture at
`0x7fd0000000-0x7fe01fffff`.

| Bus | Window |
|---|---:|
| 01 / 02 | 384 GiB (top of the CMP switch) |
| 03 / 04 | 256 GiB (GPU 05/06 branch) |
| 07 / 08 | 128 GiB (GPU 09 branch) |
| 05 / 06 / 09 | 64 GiB + 32 MiB (one CMP each) |

Final BARs:

| GPU | BAR1 | BAR3 |
|---|---|---|
| `05:00.0` | `0x1000000000-0x1fffffffff` | `0x2000000000-0x2001ffffff` |
| `06:00.0` | `0x3000000000-0x3fffffffff` | `0x4000000000-0x4001ffffff` |
| `09:00.0` | `0x5000000000-0x5fffffffff` | `0x6000000000-0x6001ffffff` |

Validated on: 3x CMP 170HX (`10de:20c2`), PEX88096, fnOS `6.18.18.c1107-trim`
(x86_64, gcc 12.2.0), cmpunlocker `--p2p` `nvidia.ko` 610.57.04,
`amd_iommu=on iommu=pt pcie_aspm=off`. Full config and logs:
[`docs/TESTED-CONFIG.md`](docs/TESTED-CONFIG.md).

## Safety and limitations

- Rewrites bridge windows and GPU BARs. A wrong topology, an occupied top
  window, or wrong PCFG offsets can hang the box until a cold reboot. Test on
  non-production hardware and keep a recovery path (older GRUB kernel, live
  USB, or removal from the initramfs).
- Run the dry-run (`mode=1`) and resource test (`mode=2`) before committing.
- Hard-coded assumptions: 64 GiB BAR1 + 32 MiB BAR3 per CMP, a 384 GiB top
  window at the fixed 64 GiB start (root window must contain it), and CMP 170HX
  (TU102) PCFG offsets. Other hardware needs adaptation.
- No DKMS - rebuild for each kernel update. Tested with `initramfs-tools`.

## Repo layout

```text
cmp_rebar_shim.c       module source
Makefile               kbuild wrapper
scripts/install.sh     install module + modprobe + initramfs
scripts/uninstall.sh   remove the above
scripts/check_boot_logs.sh  log-only boot diagnostics
scripts/verify.sh      BAR checks + CUDA P2P transfer test
tools/p2p_test.py      CUDA P2P test (ctypes, no nvcc)
docs/                  install, ordering, internals, verify, troubleshooting
etc/                   sample config snippets
```

## Contributing

AI-assisted project; if it breaks on your setup, hand an AI assistant this repo
plus your logs (`dmesg`, `lspci -tv`, `/proc/iomem`, `mode=1` dry-run) to adapt
the sizes/offsets/window to your topology, then open a PR with the diff and
before/after logs. See [`CONTRIBUTING.md`](CONTRIBUTING.md) and
[`CHANGELOG.md`](CHANGELOG.md).

## License

GPL-2.0 - see [`LICENSE`](LICENSE). NVIDIA, CMP and related marks are
trademarks of NVIDIA Corporation; this project is not affiliated with or
endorsed by NVIDIA.
