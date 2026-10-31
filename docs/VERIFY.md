# Verify

Addresses below are from the tested machine and will differ. What must hold on
yours: BAR1 = 64 GiB, BAR3 = 32 MiB, the shim runs before NVIDIA, P2P copies pass.

## One-shot

```bash
sudo ./scripts/verify.sh
```

Checks the shim is loaded, every CMP's BAR1/BAR3 size, the NVIDIA params, then runs
the CUDA P2P test. Exit 0 = all pass.

## Log-only (no nvidia-smi)

```bash
sudo ./scripts/check_boot_logs.sh
```

Healthy: all `cmp_rebar_shim` lines appear **before** `NVRM`, ending in
`NVRM: CMP BAR1: final size = 65536 MB`, with no `RmInitAdapter failed`. Early GSP
booter retries (`status=0xffff`) are fine as long as the final `normal BooterLoad
status=0x0` and `RmInitAdapter` completion appear.

## BARs

`verify.sh` detects CMP devices by PCI ID. Manual (line 2 = BAR1 = 64 GiB, line 4 =
BAR3 = 32 MiB):

```bash
sed -n '1,4p' /sys/bus/pci/devices/0000:05:00.0/resource
```

## NVIDIA params

```bash
grep -E 'EnableResizableBar|RegistryDwords' /proc/driver/nvidia/params
# EnableResizableBar: 1
# RegistryDwords: "...;RMForceStaticBar1=1;RMPcieP2PType=1"
```

`CMP BAR1: keeping stock size ... set NVreg_EnableResizableBar=1` is benign when
`RMForceStaticBar1=1` is present - the `final size = 65536 MB` line is authoritative.

## P2P transfer

```bash
python3 tools/p2p_test.py
```

Fills each device with a pattern, and for every ordered pair re-inits both buffers,
enables peer access, copies 64 MiB with `cuMemcpyPeer`, and checks the destination and
the unchanged source. Healthy = `PASS` on all pairs:

```text
pair 0 -> 1:
  PASS  64 MiB copied and verified
...
RESULT: PASS
```

## Optional: nvidia-smi

```bash
nvidia-smi topo -m
nvidia-smi topo -p2p r
nvidia-smi topo -p2p w
```

`topo -p2p` should be `OK` per pair. If `nvidia-smi` has hung, don't run it - use the
log-only check and the CUDA test instead.
