# Contributing

This is a small, hardware-specific kernel module. Changes that touch the PCI
window layout or the PCFG unlock sequence need real hardware validation.

## Before sending a change

- Build against the target kernel headers:
  `make clean && make`.
- Run `make check` (shell syntax + Python byte-compile).
- If you can, run the dry-run (`insmod ./cmp_rebar_shim.ko mode=1`) and include
  the `dmesg` plan in the change description.
- For layout/register changes, include:
  - kernel version and compiler;
  - GPU model/PCI IDs and switch topology (`lspci -tv`);
  - root prefetchable window and free ranges (`/proc/iomem`);
  - before/after `dmesg` and BAR sizes;
  - P2P test output if the change affects BAR1.
- Do not include vendor firmware, NVIDIA blobs, or cmpunlocker code.

## Style

- Kernel C style, tabs, no unrelated reformatting.
- Keep the module buildable as an out-of-tree module with the existing Makefile.
- Keep user-facing log messages prefixed with `cmp_rebar_shim:`.
- Do not hard-code a user's bus numbers or paths; derive them from the PCI tree
  and document topology assumptions.

## Safety

Changes that commit PCI resource assignments can hang a machine. Test on a
non-production system with a recovery path (older kernel / live USB).
