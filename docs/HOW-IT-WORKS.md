# How it works

## Topology (tested)

```text
00:01.1  AMD root port                 (bus 01-15)
 01:00.0  PEX88096 upstream port       (bus 02-15)
  02:00.0  downstream port             (bus 03-06)
   03:00.0  downstream port            (bus 04-06)
    04:00.0 -> 05:00.0  CMP 170HX (GPU0)
    04:10.0 -> 06:00.0  CMP 170HX (GPU1)
  02:04.0  downstream port             (bus 07-0a)
   07:00.0  downstream port            (bus 08-0a)
    08:00.0 -> 09:00.0  CMP 170HX (GPU2)
    08:10.0 -> 0a:00.0  NVMe (untouched)
```

Bus numbers are not hard-coded - the shim walks the tree from the CMP devices. The
**sizes** (64 GiB BAR1, 32 MiB BAR3, 384 GiB top window, 64 GiB alignment) are
specific to this configuration. The NVMe is safe: the shim only re-lays *prefetch*
windows and only resizes CMP BARs; the NVMe BARs are non-prefetch and left alone.

## Why the stock kernel cannot do it

A 64 GB framebuffer needs a 64 GiB BAR1, covered by every parent bridge window and
contained in the root prefetchable window. `pci_resize_resource()` is not enough:

- bridge-window sizing under-counts a 64 GiB BAR aligned at 64 GiB (the
  `size += max(r_size, align)` issue); the shim uses `ALIGN(child_size, align)`;
- `pbus_reassign_bridge_resources()` recomputes child windows but never re-runs the
  root-window allocation, so an undersized top window fails with `-ENOSPC`;
- in 6.18, once a bridge resource has a parent, `pbus_size_mem()` returns early -
  useful here, since after the shim sizes the windows the kernel writes the
  registers without overwriting the chosen sizes.

So the shim does the root allocation and bridge sizing itself, then asks the kernel
to program the windows.

## apply() (`mode=1 commit=1`)

1. `first_cmp()` finds a CMP (`10de:20c2`/`10de:2082`); `find_top_bus()` walks to the
   bus under the root port; `top_win = top_bus->resource[2]`, `root_win = top_win->parent`.
2. `bus_pref_size()` totals the prefetch size (384 GiB here); `dump_plan_walk()` logs
   the per-bridge plan.
3. `allocate_resource(root_win, ..., CMP_TOP_START, CMP_TOP_START+size-1)` checks the
   **exact fixed range** `0x1000000000-0x6fffffffff` is free, before touching anything.
4. per GPU (`prepare_gpu`): `pci_set_power_state(PCI_D0)` + 20 ms, disable memory
   decode, `unlock_rebar()` (map BAR0+PCFG, if the ReBAR cap is locked write
   `CYA 0x724=0x30`, `BAR1_CONFIG 0xDCC=0x8000000a`, re-check, set `0xBC0` size to 16
   = 2^16 MiB), release BAR1/BAR3 and set them to 64 GiB / 32 MiB with
   `MEM|PREFETCH|MEM_64|SIZEALIGN`.
5. `release_walk()` frees the old child windows; `set_sizes_walk()` sets each bridge
   to `ALIGN(child_size, align)`; `request_resource()` inserts the top window.
6. `pci_bus_assign_resources(top_bus)` assigns child BARs and windows;
   `pci_assign_unassigned_bridge_resources()` + `write_bridges_walk()` program the
   bridge registers; `enable_gpu()` re-enables memory decode and the BARs are logged.

Final tested mapping: BAR1 `0x1000000000`/`0x3000000000`/`0x5000000000`, BAR3
`0x2000000000`/`0x4000000000`/`0x6000000000` (see [`TESTED-CONFIG.md`](TESTED-CONFIG.md)).

## The NVIDIA side

The `--p2p` build sets `RMForceStaticBar1=1` (treat BAR1 as static) and
`RMPcieP2PType=1` (enable BAR1-P2P). With the shim BAR1 is already 64 GiB, so the
driver's own resize is skipped and it logs `keeping stock size` / `final size =
65536 MB`. `tools/p2p_test.py` then verifies real `cuMemcpyPeer` transfers.

## Constants

```c
#define CMP_PCFG_OFFSET      0x88000  /* BAR0 offset */
#define CMP_CYA_RESIZE_BAR1  0x724
#define CMP_BAR1_CONFIG      0xDCC
#define CMP_REBAR_CAP_BAR1   0xBBC
#define CMP_REBAR_CTRL_BAR1  0xBC0
```

ReBAR control size field = `(ctrl >> 8) & 0x3f`; 16 = 2^16 MiB = 64 GiB. These
offsets and sizes are validated values for the CMP 170HX (TU102) - verify against
your card before adapting.

## Source map

| Function | Purpose |
|---|---|
| `is_cmp` / `first_cmp` | match `10de:20c2`/`2082`; first CMP reference |
| `find_top_bus` | walk up to the bus under the root port |
| `bus_pref_size` | prefetchable size needed by a bus |
| `dump_plan_walk` | log the per-bridge plan |
| `release_walk` / `set_sizes_walk` | free / set bridge windows |
| `unlock_rebar` | PCFG unlock + 64 GiB ReBAR control |
| `prepare_gpu` / `enable_gpu` | D0, decode off, unlock, resize BARs / decode on |
| `apply` | probe, commit, assign, write bridges, verify |
| `probe_one` / `resource_test` | `mode=0` / `mode=2` |
