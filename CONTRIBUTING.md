# Contributing

Hardware-specific kernel module: changes to the PCI window layout or the PCFG unlock
sequence must be validated on real hardware.

## Before sending a change

- `make clean && make` against the target kernel headers; `make check`.
- Run the dry-run (`insmod ./cmp_rebar_shim.ko mode=1`) and include the `dmesg` plan.
- For layout/register changes, include: kernel version + compiler; GPU PCI IDs and
  topology (`lspci -tv`); root prefetch window and free ranges (`/proc/iomem`);
  before/after `dmesg` and BAR sizes; P2P output if BAR1 is affected.
- No vendor firmware, NVIDIA blobs, or cmpunlocker code.

## Style

- Kernel C style, tabs, no unrelated reformatting; keep it buildable out-of-tree with
  the existing Makefile.
- Log messages prefixed `cmp_rebar_shim:`.
- Derive bus numbers/paths from the PCI tree; do not hard-code a machine's values.

## Fixing it on your machine

If it does not work on your setup, give an AI assistant this repo plus your logs
(`dmesg`, `lspci -tv`, `/proc/iomem`, `mode=1` dry-run), have it adapt the
sizes/offsets/window to your topology, and send the result as a PR with before/after
boot logs.

`commit=1` rewrites PCI resources and can hang a machine - test on non-production
hardware with a recovery path.
