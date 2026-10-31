# Verification

The log and address samples below are from the tested machine
([`TESTED-CONFIG.md`](TESTED-CONFIG.md)) and are examples only. What must hold
on yours: BAR1 = 64 GiB, BAR3 = 32 MiB, the shim runs before NVIDIA, and the
P2P copies pass. Your bus numbers and addresses will differ.

## 1. Log-only check (no nvidia-smi)

```bash
sudo ./scripts/check_boot_logs.sh
```

This prints:

- kernel and uptime;
- loaded `nvidia*` / `cmp_rebar_shim` modules;
- D-state processes;
- `cmp_rebar_shim` dmesg lines;
- critical NVRM lines (GSP success, RmInitAdapter, CMP BAR1, Xid);
- CMP BAR1/BAR3 sizes (auto-detected by PCI ID);
- `EnableResizableBar` / `RegistryDwords`;
- recent journal warnings.

### Healthy shim log

```text
cmp_rebar_shim: apply commit=1 top bus 01 bridge 0000:00:01.1
cmp_rebar_shim: root window 0xe40000000-0x7fffffffff, top needs 0x6000000000
...
cmp_rebar_shim: 0000:05:00.0: REBAR cap locked (0x400), unlocking
cmp_rebar_shim: 0000:05:00.0: REBAR ctrl now 0x1001 (size=16)
...
cmp_rebar_shim: top insert 0x1000000000-0x6fffffffff -> 0
cmp_rebar_shim: pci_bus_assign_resources done
cmp_rebar_shim: bridge registers written
cmp_rebar_shim: verify 0000:05:00.0 BAR1 0x1000000000-0x1fffffffff len=0x1000000000 flags=0x14220c ...
cmp_rebar_shim: verify 0000:06:00.0 BAR1 0x3000000000-0x3fffffffff len=0x1000000000 flags=0x14220c ...
cmp_rebar_shim: verify 0000:09:00.0 BAR1 0x5000000000-0x5fffffffff len=0x1000000000 flags=0x14220c ...
```

`REBAR cap locked (0x400), unlocking` is expected on a cold boot. It means the
shim is performing the unlock that the NVIDIA driver has not done yet.

### Healthy NVIDIA log

```text
NVRM: CMP BAR1: final size = 65536 MB
```

There must be **no**:

```text
NVRM: ... RmInitAdapter failed!
NVRM: ... Cannot initialize GSP firmware RM
```

Early GSP booter retries (`status=0xffff` on some attempts) can happen on a
healthy cold boot as long as the final line is:

```text
NVRM: GPU2 kgspBootstrap_TU102: SEC2_DEBUG: normal BooterLoad status=0x0
```

and `RmInitAdapter` completes.

## 2. BAR checks

`scripts/verify.sh` checks the CMP devices automatically. Manual equivalent:

```bash
for d in /sys/bus/pci/devices/0000:0{5,6,9}:00.0; do
    echo "== $d =="
    sed -n '1,4p' "$d/resource"
done
```

BAR1 is line 2 (64 GiB = `0x1000000000`), BAR3 is line 4 (32 MiB =
`0x2000000`).

Expected (tested addresses):

| GPU | BAR1 | BAR3 |
|---|---|---|
| `05:00.0` | `0x1000000000-0x1fffffffff` | `0x2000000000-0x2001ffffff` |
| `06:00.0` | `0x3000000000-0x3fffffffff` | `0x4000000000-0x4001ffffff` |
| `09:00.0` | `0x5000000000-0x5fffffffff` | `0x6000000000-0x6001ffffff` |

The addresses can differ if your top window allocation differs; the **sizes**
must be 64 GiB and 32 MiB.

## 3. NVIDIA parameters

```bash
grep -E 'EnableResizableBar|RegistryDwords' /proc/driver/nvidia/params
```

Expected cmpunlocker `--p2p` build:

```text
EnableResizableBar: 1
RegistryDwords: "...;RMForceStaticBar1=1;RMPcieP2PType=1"
```

The message

```text
NVRM: CMP BAR1: keeping stock size (dynamic mapping); set NVreg_EnableResizableBar=1 to resize
```

is benign when `RMForceStaticBar1=1` is present; the driver keeps the shim's
static 64 GiB window. Check the following `final size = 65536 MB`.

## 4. Real P2P transfer test

```bash
python3 tools/p2p_test.py
```

The test:

- enumerates all CUDA devices;
- fills each device with a different 32-bit pattern and verifies it;
- for every ordered pair, reinitializes source and destination patterns,
  enables peer access, runs `cuMemcpyPeer` for 64 MiB, verifies the destination,
  and verifies the source is unchanged.

Healthy output:

```text
CUDA devices: 3
  dev 0: NVIDIA CMP 170HX         0000:05:00.0  64910 MiB
  dev 1: NVIDIA CMP 170HX         0000:06:00.0  64910 MiB
  dev 2: NVIDIA CMP 170HX         0000:09:00.0  64910 MiB
self-check (each device holds its own pattern):
  dev 0: OK
  dev 1: OK
  dev 2: OK
pair 0 -> 1:
  PASS  64 MiB copied and verified
...
pair 2 -> 1:
  PASS  64 MiB copied and verified
RESULT: PASS
```

`DEST MISMATCH` on a pair that reuses buffers from a previous pair is a test bug,
not necessarily a hardware failure. This repository's test reinitializes both
buffers before each pair, so it does not have that problem.

## 5. Optional: nvidia-smi

If the driver is healthy, these are the standard checks:

```bash
nvidia-smi --query-gpu=index,pci.bus_id,memory.total --format=csv
nvidia-smi topo -m
nvidia-smi topo -p2p r
nvidia-smi topo -p2p w
```

`topo -p2p` should report `OK` for every pair. If `nvidia-smi` hangs, do not
keep running it: unload NVIDIA or cold reboot, and use the log-only check plus
the CUDA test instead.

## 6. One-shot verify

```bash
sudo ./scripts/verify.sh
```

It checks that the shim is loaded, verifies every CMP BAR1/BAR3 size, reads the
NVIDIA parameters, and runs the CUDA P2P test. Exit code 0 means all checks
passed.
