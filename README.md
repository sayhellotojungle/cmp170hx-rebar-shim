# cmp-rebar-shim

A small out-of-tree Linux kernel module that re-lays-out the PCIe prefetchable
windows in front of NVIDIA CMP 170HX GPUs and gives each card a **64 GiB BAR1**
plus a 32 MiB BAR3 **before the proprietary NVIDIA driver probes the devices**.
No kernel patch, no custom kernel image - one module in the initramfs.

> Research / enthusiast project. It moves PCI bridge windows and writes
> vendor-specific GPU registers. Read [Safety](#safety) first. Not affiliated
> with NVIDIA. Licensed GPL-2.0.

> **Written with AI.** This module and its docs were generated with an AI
> assistant and validated end-to-end on real hardware
> ([`docs/TESTED-CONFIG.md`](docs/TESTED-CONFIG.md)).

> **About the numbers in this repository.** Bus numbers, addresses, window
> sizes, register offsets and installer flags shown in the README and `docs/`
> are from one tested machine (`docs/TESTED-CONFIG.md`). They are examples and
> log samples, not requirements. What is fixed by the project's purpose is the
> *result*: 64 GiB BAR1 + 32 MiB BAR3 per CMP 170HX before `nvidia.ko` probes.
> Everything else depends on your firmware, your switch tree and your
> distribution. Always run the dry-run first and use *your* `dmesg` output.

## What problem does it solve?

**The core problem.** On rigs where the GPUs sit behind a PCIe switch (the
tested tree uses a PEX88096), enumeration assigns each GPU only a 64 MiB
BAR1, and Resizable BAR cannot rescue that. Growing the BAR also requires
growing every bridge window in front of the GPU, and the stock kernel cannot
re-plan that window tree:

- the stock bridge-window sizing logic does not account for the child BAR
  alignment correctly (a 64 GiB BAR aligned at 64 GiB needs more window than
  the kernel reserves), and
- the top-level prefetchable window is never recomputed when child windows
  grow.

So the resize fails. When a driver *requires* the large BAR - the
[cmpunlocker](docs/CMPUNLOCKER.md) `--p2p` `nvidia.ko` needs a 64 GiB static
BAR1 to map the unlocked 64 GB framebuffer and do BAR1-P2P - the failure
cascades into a hang at probe:

| Symptom | What happened |
|---|---|
| `NVRM: ... failed to execute Booter: status 0x65`, `RmInitAdapter failed!`; `nvidia-smi` hangs in `D` state | the `--p2p` driver probed with a 64 MiB BAR1 and GSP initialization failed |
| `cuDeviceCanAccessPeer()` returns 0; `nvidia-smi topo -p2p` = `GNS` | the driver kept the 64 MiB dynamic BAR1; P2P stays off |

**Why a shim instead of patching the kernel.** The direct fix is a kernel
patch teaching the PCI core to re-plan the prefetchable window tree. That is
heavy: a custom kernel to build and re-build on every update, toolchain
requirements, and - the real blocker - on appliance-style systems that ship
prebuilt, trimmed kernels with no kernel development path (fnOS / 飞牛OS being
the one this project was born on) it simply is not an option.

This shim reaches the same end state from the outside: one small module,
loaded from the initramfs before any driver probes, using only the exported
resource APIs of the **stock kernel**. No kernel source, no patch, no custom
image - and it works on a patched-kernel-free distro just as well. With the
shim:

- each GPU gets BAR1 = 64 GiB, BAR3 = 32 MiB, switch bridge windows re-planned;
- the driver logs `NVRM: CMP BAR1: final size = 65536 MB` and GSP initializes;
- P2P is verified with real 64 MiB `cuMemcpyPeer` transfers in every ordered
  pair ([`tools/p2p_test.py`](tools/p2p_test.py)).

What the shim does **not** do:

- it does not unlock the 64 GB framebuffer (cmpunlocker's job);
- it does not implement BAR1-P2P (cmpunlocker `--p2p`'s job);
- it is not a generic ReBAR tool: the sizes and PCFG register offsets are
  specific to the CMP 170HX and the tested switch tree.

## Usage

### With cmpunlocker `--p2p` (the validated path)

Install order and boot order both matter. The cmpunlocker installer
regenerates the initramfs, so install the shim **after** it. At boot, the shim
must run from the initramfs **before** the NVIDIA driver probes the GPUs; the
installer enforces this (it refuses if `nvidia` is listed before the shim in
`/etc/initramfs-tools/modules`).

```bash
# 1. cmpunlocker with --p2p first (third-party; see docs/CMPUNLOCKER.md)
#    The flags below are only what the tested host used - follow the
#    documentation of your cmpunlocker version.
cd /path/to/cmpunlocker
sudo ./install.sh --p2p --profile=8gb --no-passthrough --no-gen2-service --no-iommu

# 2. this shim
cd cmp-rebar-shim
make
sudo ./scripts/install.sh

# 3. cold reboot (power cycle, not just a warm reboot)

# 4. verify
sudo ./scripts/verify.sh        # shim loaded + BAR sizes + real CUDA P2P copies
```

What step 2 does (see [`docs/INSTALL.md`](docs/INSTALL.md) for each step and
the manual/dracut equivalents):

- copies `cmp_rebar_shim.ko` to `/lib/modules/$(uname -r)/extra/`;
- writes `options cmp_rebar_shim mode=1 commit=1` to
  `/etc/modprobe.d/cmp-rebar-shim.conf`;
- appends `cmp_rebar_shim` to `/etc/initramfs-tools/modules`;
- runs `depmod` and `update-initramfs -u -k $(uname -r)`.

Notes:

- If you installed the shim **before** cmpunlocker, re-run
  `sudo ./scripts/install.sh` after cmpunlocker (its installer rebuilds the
  initramfs), then cold reboot.
- If NVIDIA is already loaded with the stock 64 MiB BAR1, loading the shim on
  top will not fix that boot; a cold reboot is required so the shim runs
  first.
- Full ordering rationale, flags, and pitfalls:
  [`docs/CMPUNLOCKER.md`](docs/CMPUNLOCKER.md).

### Before installing: check the plan (changes nothing)

```bash
make

# probe: GPUs, BARs, ReBAR caps, bridge windows (read-only)
sudo insmod ./cmp_rebar_shim.ko mode=0
dmesg | tail -80
sudo rmmod cmp_rebar_shim

# resource test: can a 384 GiB window fit anywhere? (in-memory only)
sudo insmod ./cmp_rebar_shim.ko mode=2
dmesg | tail -40
sudo rmmod cmp_rebar_shim

# dry-run of the real layout: prints the per-bridge plan and verifies that
# the fixed top window fits in the root prefetchable window. Writes nothing.
sudo insmod ./cmp_rebar_shim.ko mode=1
dmesg | tail -80
sudo rmmod cmp_rebar_shim
```

In the dry-run log check: `root window`, `top needs` (the per-card total:
384 GiB for the tested 3-card tree), `top allocation test: ret=0`, and the
per-GPU `-> 64G/32M` lines. If the plan is wrong for your topology, do not
install; see [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md).

Manual live test (only when the NVIDIA driver is not loaded):

```bash
sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia 2>/dev/null || true
sudo insmod ./cmp_rebar_shim.ko mode=1 commit=1
dmesg | tail -60          # must show "REBAR ctrl now ..." and 64G BARs
sudo modprobe nvidia
sudo modprobe nvidia-uvm
```

If the shim fails, do **not** load NVIDIA with the P2P dwords; the driver will
try to use a 64 MiB BAR1 and GSP initialization will fail. Cold reboot instead.

## Verification

Log-only (does not call `nvidia-smi`):

```bash
sudo ./scripts/check_boot_logs.sh
```

Expected on a healthy cold boot (addresses are from the tested machine; yours
will differ — the **sizes** and the shim-before-NVIDIA order are what matter):

```text
cmp_rebar_shim: 0000:05:00.0: REBAR cap locked (0x400), unlocking
cmp_rebar_shim: 0000:05:00.0: REBAR ctrl now 0x1001 (size=16)
cmp_rebar_shim: verify 0000:05:00.0 BAR1 0x1000000000-0x1fffffffff len=0x1000000000 flags=0x14220c ...
...
NVRM: CMP BAR1: final size = 65536 MB
```

The message

```text
NVRM: CMP BAR1: keeping stock size (dynamic mapping); set NVreg_EnableResizableBar=1 to resize
```

can appear even when `EnableResizableBar: 1`; with `RMForceStaticBar1=1` the
driver keeps the shim's static window. The next `final size = 65536 MB` line is
authoritative.

Real P2P transfer test (CUDA driver API, no `nvidia-smi`, no nvcc):

```bash
python3 tools/p2p_test.py      # or via sudo ./scripts/verify.sh
```

See [`docs/VERIFY.md`](docs/VERIFY.md) for the full expected output.

## Status

Validated end-to-end on the configuration in
[`docs/TESTED-CONFIG.md`](docs/TESTED-CONFIG.md):

- 3x NVIDIA CMP 170HX (`10de:20c2`), 64 GB framebuffer unlock
- Broadcom PEX88096 PCIe switch
- fnOS kernel `6.18.18.c1107-trim` (x86_64), gcc 12.2.0
- cmpunlocker `--p2p` `nvidia.ko` 610.57.04
- `amd_iommu=on iommu=pt pcie_aspm=off`
- cold boot -> shim unlocks BAR1 -> NVIDIA GSP initializes -> CUDA P2P works in
  all six ordered directions (64 MiB `cuMemcpyPeer` transfers verified)

## How it works

The shim is loaded from the initramfs, so it runs before root is mounted and
before udev loads `nvidia.ko`. On `mode=1 commit=1` it:

1. finds the CMP GPUs (PCI IDs `10de:20c2` / `10de:2082`) and walks the bus
   tree to the top bridge of the switch;
2. computes the prefetchable window each bridge needs (64 GiB + 32 MiB per GPU)
   and checks that the fixed 384 GiB top window (starting at 64 GiB) fits in
   the root prefetchable window;
3. for every GPU: forces D0, waits for the PCFG block, unlocks ReBAR through
   BAR0 MMIO (`CYA=0x30`, `BAR1_CONFIG=0x8000000a`, ReBAR control size 16),
   releases the old BAR1/BAR3 resources and re-sizes them to 64 GiB / 32 MiB;
4. releases the old child windows, writes the new bridge sizes, inserts the top
   window, runs `pci_bus_assign_resources()`, programs the bridge registers,
   re-enables memory decode and verifies the result.

It does **not** modify the kernel image, the distro prebuilt modules, or the
NVIDIA driver. It uses exported PCI resource APIs and BAR0 PCFG MMIO. The
driver then probes the GPUs with BAR1 already at 64 GiB; with
`RMForceStaticBar1=1` it adopts the shim's static window.

See [`docs/HOW-IT-WORKS.md`](docs/HOW-IT-WORKS.md) for the technical details.

## Layout (tested tree)

Root prefetchable window: `0xe40000000-0x7fffffffff`. The shim uses the top
window `0x1000000000-0x6fffffffff` (384 GiB) below the iGPU aperture at
`0x7fd0000000-0x7fe01fffff`.

| Switch bus | Window size | Notes |
|---|---:|---|
| 01 / 02 | 384 GiB | top of the CMP switch |
| 03 / 04 | 256 GiB | GPU 05/06 branch |
| 07 / 08 | 128 GiB | GPU 09 branch |
| 05 / 06 / 09 | 64 GiB + 32 MiB | one CMP per bus |

Final GPU BARs after a cold boot:

| GPU | BAR1 | BAR3 |
|---|---|---|
| `05:00.0` | `0x1000000000-0x1fffffffff` | `0x2000000000-0x2001ffffff` |
| `06:00.0` | `0x3000000000-0x3fffffffff` | `0x4000000000-0x4001ffffff` |
| `09:00.0` | `0x5000000000-0x5fffffffff` | `0x6000000000-0x6001ffffff` |

Your bus numbers and free space will differ. Use the dry-run (`mode=1` without
`commit=1`) first and inspect the plan in `dmesg`.

## Safety

This module rewrites PCI bridge windows and GPU BARs. A wrong topology, an
occupied top window, or wrong PCFG offsets can hang the machine or leave GPUs
unusable until a cold reboot.

- Test on a non-production machine first. Back up data.
- The shim only re-plays the **prefetchable** windows and only touches CMP
  devices; the NVMe port behind the tested switch (`08:10.0 -> 0a:00.0`) was
  unaffected. Still verify your topology before setting `commit=1`.
- Use the dry-run (`mode=1`, no `commit=1`) and the resource test (`mode=2`)
  first.
- The PCFG offsets and 64 GiB/32 MiB sizes are for the CMP 170HX (TU102) in the
  tested tree. Other cards/topologies need adaptation.
- Keep a recovery path: an older kernel entry in GRUB, a live USB, or a way to
  remove the shim from the initramfs.
- No warranty. GPL-2.0.

## Limitations

- **Topology-specific.** The code computes bridge sizes by walking the bus
  tree, but assumes 64 GiB BAR1 + 32 MiB BAR3 per CMP, a 384 GiB top window at
  the fixed 64 GiB start, and the tested PCFG offsets.
- The root prefetchable window must contain `0x1000000000` for 384 GiB
  (64 GiB-aligned).
- It does **not** unlock the GPU's framebuffer memory; that is cmpunlocker's
  job.
- It does **not** implement BAR1-P2P; that is cmpunlocker's `--p2p` mode's
  job.
- No DKMS yet. Rebuild for every kernel update.
- Tested with `initramfs-tools`. For dracut, see
  [`docs/INSTALL.md`](docs/INSTALL.md).

## Repository layout

```text
cmp_rebar_shim.c        module source
Makefile                kbuild wrapper
scripts/install.sh      install module + modprobe + initramfs
scripts/uninstall.sh    remove the above
scripts/check_boot_logs.sh  log-only boot diagnostics (no nvidia-smi)
scripts/verify.sh       BAR checks + real CUDA P2P transfer test
tools/p2p_test.py       CUDA driver API P2P test (ctypes, no nvcc)
docs/                   install, ordering, internals, verification, troubleshooting
etc/                    sample config snippets
```

## Contributing

This project was written with AI assistance and tested on real hardware, but
environments vary a lot. If it does not work on your machine, the intended
workflow is: point an AI assistant at this repository plus your logs (`dmesg`,
`lspci -tv`, `/proc/iomem`, and the `mode=1` dry-run output), let it read the
code and adjust the sizes, register offsets and top-window placement to your
topology, then open a PR with the diff and your before/after boot logs so the fix
lands here too. That is a welcome contribution.

See [`CONTRIBUTING.md`](CONTRIBUTING.md). Layout/register changes need real
hardware validation and boot logs. See [`CHANGELOG.md`](CHANGELOG.md) for the
version history.

## License

GPL-2.0. See [`LICENSE`](LICENSE).

NVIDIA, CMP, and related marks are trademarks of NVIDIA Corporation. This
project is not affiliated with or endorsed by NVIDIA.
