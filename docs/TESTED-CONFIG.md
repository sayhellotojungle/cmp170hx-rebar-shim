# Tested configuration

Validated on 2026-10-30. This is one machine's working example, not a standard:
your bus numbers, addresses, free space, kernel, flags and BIOS layout will
differ.

## Hardware

| Item | Value |
|---|---|
| GPUs | 3x NVIDIA CMP 170HX (`10de:20c2`), 64 GB framebuffer unlock |
| PCIe switch | Broadcom PEX88096 |
| Root port | AMD `00:01.1` |
| GPU BDFs | `05:00.0`, `06:00.0`, `09:00.0` |

## Software

| Item | Value |
|---|---|
| OS | fnOS (Debian-based) |
| Kernel | `6.18.18.c1107-trim` x86_64 |
| Compiler | gcc 12.2.0 (Debian 12.2.0-14) |
| NVIDIA driver | cmpunlocker `--p2p` build, 610.57.04 |
| cmpunlocker `--p2p` flags | `--p2p --profile=8gb --no-passthrough --no-iommu` |
| IOMMU | `amd_iommu=on iommu=pt` |
| Other cmdline | `pcie_aspm=off`, `modprobe.blacklist=pcspkr` (unrelated) |
| initramfs | `initramfs-tools` |
| shim version | 0.1.2 (boot logs below captured on 0.1.1; the successful commit path is identical) |

## NVIDIA registry dwords

```text
EnableResizableBar: 1
RegistryDwords: "RmForceEnableGen2=1;RMPcieLinkSpeed=0x1;RMForceStaticBar1=1;RMPcieP2PType=1"
```

`/etc/modprobe.d/cmp-pcie-gen2.conf`:

```ini
options nvidia NVreg_RegistryDwords="RmForceEnableGen2=1;RMPcieLinkSpeed=0x1;RMForceStaticBar1=1;RMPcieP2PType=1"
```

`/etc/modprobe.d/cmp-rebar-shim.conf`:

```ini
options cmp_rebar_shim mode=1 commit=1
```

## Window layout

Root prefetchable window: `0xe40000000-0x7fffffffff`. The iGPU aperture
`0x7fd0000000-0x7fe01fffff` is already occupied.

| Switch bus | Planned size | Notes |
|---|---:|---|
| 01 / 02 | 384 GiB | top of the CMP switch |
| 03 / 04 | 256 GiB | GPU 05/06 branch |
| 07 / 08 | 128 GiB | GPU 09 branch |
| 05 / 06 / 09 | 64 GiB + 32 MiB | one CMP per bus |

Top window: `0x1000000000-0x6fffffffff` (384 GiB, 64 GiB aligned).

Final BARs:

| GPU | BAR1 | BAR3 |
|---|---|---|
| `05:00.0` | `0x1000000000-0x1fffffffff` | `0x2000000000-0x2001ffffff` |
| `06:00.0` | `0x3000000000-0x3fffffffff` | `0x4000000000-0x4001ffffff` |
| `09:00.0` | `0x5000000000-0x5fffffffff` | `0x6000000000-0x6001ffffff` |

Flags: `0x14220c` (`MEM | PREFETCH | MEM_64 | SIZEALIGN`, plus the resource
flags set by the PCI core).

## Boot log (trimmed)

```text
[    5.234229] cmp_rebar_shim: loading out-of-tree module taints kernel.
[    5.234444] cmp_rebar_shim: apply commit=1 top bus 01 bridge 0000:00:01.1
[    5.234449] cmp_rebar_shim: root window 0xe40000000-0x7fffffffff, top needs 0x6000000000
...
[    5.234490] cmp_rebar_shim: top allocation test: ret=0 0x1000000000-0x6fffffffff
[    5.260249] cmp_rebar_shim: 0000:05:00.0: REBAR cap locked (0x400), unlocking
[    5.260257] cmp_rebar_shim: 0000:05:00.0: REBAR ctrl now 0x1001 (size=16)
[    5.288244] cmp_rebar_shim: 0000:06:00.0: REBAR cap locked (0x400), unlocking
[    5.288251] cmp_rebar_shim: 0000:06:00.0: REBAR ctrl now 0x1001 (size=16)
[    5.312244] cmp_rebar_shim: 0000:09:00.0: REBAR cap locked (0x400), unlocking
[    5.312251] cmp_rebar_shim: 0000:09:00.0: REBAR ctrl now 0x1001 (size=16)
[    5.312270] cmp_rebar_shim: top insert 0x1000000000-0x6fffffffff -> 0
[    5.312391] cmp_rebar_shim: pci_bus_assign_resources done
[    5.444273] cmp_rebar_shim: bridge registers written
[    5.444284] cmp_rebar_shim: verify 0000:05:00.0 BAR1 0x1000000000-0x1fffffffff len=0x1000000000 flags=0x14220c BAR3 0x2000000000-0x2001ffffff len=0x2000000
[    5.444290] cmp_rebar_shim: verify 0000:06:00.0 BAR1 0x3000000000-0x3fffffffff len=0x1000000000 flags=0x14220c BAR3 0x4000000000-0x4001ffffff len=0x2000000
[    5.444297] cmp_rebar_shim: verify 0000:09:00.0 BAR1 0x5000000000-0x5fffffffff len=0x1000000000 flags=0x14220c BAR3 0x6000000000-0x6001ffffff len=0x2000000
...
[   14.590864] NVRM: GPU2 kgspExecuteBooterLoad_TU102: failed to execute Booter Load: 0xffff
[   15.063670] NVRM: GPU2 kgspBootstrap_TU102: SEC2_DEBUG: normal BooterLoad status=0x0
[   15.632899] NVRM: GPU2 _kgspInitGpuProperties: SEC2_DEBUG: static-info BEFORE: fb_length=0x1000000000 numRegions=5
[   15.827526] NVRM: GPU2 RmInitAdapter: SEC2_DEBUG: late PMA extension status=0x0
```

Note the **order**: shim at ~5 s, NVIDIA at ~14 s. The early GPU2 booter retries
are normal; the final `normal BooterLoad status=0x0` and `RmInitAdapter`
completion are what matter.

## P2P transfer result

`python3 tools/p2p_test.py`:

```text
CUDA devices: 3
  dev 0: NVIDIA CMP 170HX         0000:05:00.0  64910 MiB
  dev 1: NVIDIA CMP 170HX         0000:06:00.0  64910 MiB
  dev 2: NVIDIA CMP 170HX         0000:09:00.0  64910 MiB
self-check (each device holds its own pattern):
  dev 0: OK
  dev 1: OK
  dev 2: OK
pair 0 -> 1: PASS  64 MiB copied and verified
pair 0 -> 2: PASS  64 MiB copied and verified
pair 1 -> 0: PASS  64 MiB copied and verified
pair 1 -> 2: PASS  64 MiB copied and verified
pair 2 -> 0: PASS  64 MiB copied and verified
pair 2 -> 1: PASS  64 MiB copied and verified
RESULT: PASS
```

`nvidia-smi topo -p2p r` and `-p2p w` report `OK` for all pairs.
`nvidia-smi topo -m` reports `PIX` between GPU0/GPU1 and `PXB` between GPU0/1 and
GPU2.

## Notes

- The driver logs `CMP BAR1: keeping stock size ... set NVreg_EnableResizableBar=1
  to resize` even with `EnableResizableBar: 1`; with `RMForceStaticBar1=1` it
  keeps the shim's 64 GiB static window. The `final size = 65536 MB` line is
  authoritative.
- The tested initramfs did **not** include `nvidia.ko`; NVIDIA was loaded by udev
  after root. Verify this on your system (`lsinitramfs` / `lsinitrd`).
- The shim's `REBAR cap locked (0x400), unlocking` line appears only on a cold
  boot (or after the GPU has lost power); after the NVIDIA driver has run, the
  capability is already unlocked and the line is omitted.
