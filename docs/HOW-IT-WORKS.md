# How it works

## The topology

The tested machine has three CMP 170HX cards behind a Broadcom PEX88096 switch
(`lspci -tv`):

```text
00:01.1  AMD root port                    (secondary bus 01-15)
 01:00.0  PEX88096 upstream port          (bus 02-15)
  02:00.0  downstream port                (bus 03-06)
   03:00.0  downstream port               (bus 04-06)
    04:00.0  downstream port       -> 05:00.0  CMP 170HX (GPU0)
    04:10.0  downstream port       -> 06:00.0  CMP 170HX (GPU1)
  02:04.0  downstream port                (bus 07-0a)
   07:00.0  downstream port               (bus 08-0a)
    08:00.0  downstream port       -> 09:00.0  CMP 170HX (GPU2)
    08:10.0  downstream port       -> 0a:00.0  NVMe (88NR2241, untouched)
```

The exact bus numbers are not hard-coded in the layout logic; the shim walks the
bus tree from the CMP devices. The **sizes** (64 GiB BAR1, 32 MiB BAR3, 384 GiB
top window, 64 GiB alignment) are specific to this configuration. The NVMe
behind `08:10.0` is unaffected: the shim only re-lays the **prefetchable**
windows and only resizes CMP BARs, and the NVMe BARs are non-prefetchable and
stay assigned.

## Why the stock kernel does not do this

A 64 GB framebuffer needs a 64 GiB BAR1. The bridge windows in front of the GPUs
must cover that BAR, and the root prefetchable window must contain the bridge's
top window.

The normal `pci_resize_resource()` path is not sufficient here:

1. The kernel's bridge-window sizing uses the child size and alignment in a way
   that under-estimates the window needed for a 64 GiB BAR aligned at 64 GiB
   (the familiar `size += max(r_size, align)` issue). The shim uses
   `ALIGN(child_size, child_align)` explicitly.
2. `pbus_reassign_bridge_resources()` recomputes the **child** windows but does
   not re-run the top-level window allocation from the root bus. If the top
   window is too small, resizing fails with `-ENOSPC`.
3. In the tested kernel (6.18), once a bridge resource has a parent
   (`b_res->parent != NULL`), `pbus_size_mem()` returns early. That is useful for
   the shim: after the shim has sized the windows,
   `pci_assign_unassigned_bridge_resources()` writes the bridge registers but
   does not overwrite the chosen sizes.

So the shim does the top-level allocation and the bridge sizing itself, then
asks the kernel to program the windows.

## Why a shim instead of a kernel patch

A kernel patch teaching `pbus_size_mem()`/root-window allocation to re-plan
the whole tree would fix the sizing in place, but it means building and
re-building a custom kernel for every update, with a matching toolchain - and
on appliance-style systems that ship prebuilt, trimmed kernels with no kernel
development path (fnOS / 飞牛OS being the one this project was born on) it is
not possible at all. The shim produces the same end state from the outside,
using only the exported resource APIs of the stock kernel, at initramfs time,
before any driver probes. Nothing in the boot chain needs the kernel source.

## The shim algorithm

`apply()` in `cmp_rebar_shim.c`:

1. `first_cmp()` finds a CMP GPU (`10de:20c2` / `10de:2082`).
2. `find_top_bus()` walks up to the bus below the root port.
3. `top_win = top_bus->resource[2]` is the prefetchable window of the top bus;
   `root_win = top_win->parent` is the root prefetchable window.
4. `bus_pref_size()` computes the total prefetchable size the top bus needs
   (384 GiB in the tested tree). `dump_plan_walk()` logs the plan for every
   bridge.
5. `allocate_resource(root_win, probe, top_size, CMP_TOP_START,
   CMP_TOP_START + top_size - 1, ...)` checks that the **exact fixed range**
   the commit will use, `0x1000000000-0x6fffffffff` (384 GiB, 64 GiB aligned),
   is free inside the root prefetchable window, below the iGPU aperture at
   `0x7fd0000000-0x7fe01fffff`. The check runs before any GPU or bridge is
   touched, so a failure leaves the machine untouched.
6. For every CMP GPU, `prepare_gpu(dev, true)`:
   - `pci_set_power_state(dev, PCI_D0)`, then `msleep(20)` so the PCFG block is
     accessible even when the GPU was in D3cold at initramfs time;
   - disables memory decode while changing BARs;
   - `unlock_rebar(dev)`:
     - maps BAR0 + `0x88000` (PCFG);
     - reads the BAR1 ReBAR capability at PCFG `0xBBC`; if it is not unlocked
       (bit 20 clear, stock value `0x400`), writes the unlock sequence:
       - `0x724` (CYA resize BAR1) = `0x30`
       - `0xDCC` (BAR1 config) = `0x8000000a`
     - re-reads `0xBBC` and requires bit 20;
     - reads `0xBC0` (ReBAR control), clears the size field, sets size `16`
       (2^16 MiB = 64 GiB);
   - releases the old BAR1/BAR3 resources and sets them to 64 GiB / 32 MiB with
     `IORESOURCE_MEM | PREFETCH | MEM_64 | SIZEALIGN`.
7. `release_walk()` releases the old child windows; `set_sizes_walk()` sets each
   bridge window to `ALIGN(child_size, child_align)`.
8. `request_resource(root_win, top_win)` inserts the top window.
9. `pci_bus_assign_resources(top_bus)` assigns the child BARs and bridge
   windows; `pci_assign_unassigned_bridge_resources()` and
   `write_bridges_walk()` program the bridge registers.
10. `enable_gpu()` re-enables memory decode and the final BARs are logged.

The final tested mapping:

| Device | BAR1 | BAR3 |
|---|---|---|
| `05:00.0` | `0x1000000000-0x1fffffffff` (64 GiB) | `0x2000000000-0x2001ffffff` (32 MiB) |
| `06:00.0` | `0x3000000000-0x3fffffffff` | `0x4000000000-0x4001ffffff` |
| `09:00.0` | `0x5000000000-0x5fffffffff` | `0x6000000000-0x6001ffffff` |

## Why the shim must run before NVIDIA

The NVIDIA driver reads the BAR1 resource size when it probes the GPU and builds
its static BAR1 mapping and BAR1-P2P mappings from it. If it sees 64 MiB, the
P2P path either reports "GPU not supported" or fails GSP initialization.

The shim is installed in the initramfs:

```text
initramfs:
  cmp_rebar_shim.ko          <- loaded before root is mounted
...
root filesystem:
  udev -> nvidia.ko          <- loaded later, sees BAR1 = 64 GiB
```

That is why the module is added to `/etc/initramfs-tools/modules` and why
`nvidia` must not be listed before it (or at all) in that file.

## The NVIDIA side

The cmpunlocker `--p2p` build uses:

```ini
RMForceStaticBar1=1
RMPcieP2PType=1
```

`RMForceStaticBar1=1` makes the driver treat the existing BAR1 as a static
window. With the shim, `pci_resource_len(BAR1)` is already 64 GiB, so the
driver's own resize path sees `old_size == requested_size` and skips. The log
line

```text
NVRM: CMP BAR1: keeping stock size (dynamic mapping); set NVreg_EnableResizableBar=1 to resize
```

can therefore appear even with `EnableResizableBar: 1`; `final size = 65536 MB`
is the authoritative result.

`RMPcieP2PType=1` enables the BAR1-P2P mapping. In the tested tree,
`nvidia-smi topo -p2p r/w` reports `OK` for all pairs and the CUDA test in
`tools/p2p_test.py` verifies real 64 MiB `cuMemcpyPeer` transfers.

## Boot-time failure mode (fixed in 0.1.1)

An earlier version required the ReBAR capability to be **already unlocked**
before writing the unlock sequence. That is true after the NVIDIA driver has
probed the GPU, but not at initramfs time, where the stock value is `0x400`.
The shim aborted with:

```text
cmp_rebar_shim: 0000:05:00.0: REBAR cap not unlocked (0x400)
```

leaving BAR1 at 64 MiB. NVIDIA then failed GSP initialization:

```text
NVRM: GPU0 s_executeBooterUcode_TU102: failed to execute Booter: status 0x65
NVRM: GPU0 RmInitAdapter: Cannot initialize GSP firmware RM
NVRM: GPU 0000:05:00.0: RmInitAdapter failed!
```

Current versions write the unlock sequence unconditionally (after checking that
the PCFG region is readable) and verify the result.

## Source map

| Function | Purpose |
|---|---|
| `is_cmp()` | match `10de:20c2` / `10de:2082` |
| `find_top_bus()` | walk up to the bus under the root port |
| `bus_pref_size()` | compute prefetchable size for a bus |
| `dump_plan_walk()` | log the per-bridge plan |
| `release_walk()` | release old child windows |
| `set_sizes_walk()` | set bridge windows to `ALIGN(child_size, child_align)` |
| `unlock_rebar()` | PCFG unlock + 64 GiB ReBAR control |
| `prepare_gpu()` | D0, disable decode, unlock, set BAR1/BAR3 resources |
| `enable_gpu()` | re-enable memory decode |
| `apply()` | top-level probe, commit, assign, write bridges, verify |
| `probe_one()` | read-only probe mode |
| `resource_test()` | mode=2 in-memory resource test |

## Constants

```c
#define CMP_PCFG_OFFSET      0x88000  /* BAR0 offset */
#define CMP_CYA_RESIZE_BAR1  0x724
#define CMP_BAR1_CONFIG      0xDCC
#define CMP_REBAR_CAP_BAR1   0xBBC
#define CMP_REBAR_CTRL_BAR1  0xBC0
```

The ReBAR control size field is `(ctrl >> 8) & 0x3f`; value 16 means
2^16 MiB = 64 GiB.

These offsets and sizes are validated values for the CMP 170HX (TU102) and are
examples: verify them against your card before adapting the module. All
addresses and bus numbers elsewhere in this document are from the tested
machine and are illustrative.
