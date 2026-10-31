# Changelog

## 0.1.2 - 2026-10-30

- Docs: point at a `--p2p`-capable cmpunlocker - recommend
  `github.com/bayley/cmpunlocker` first, with the maintained fork
  `github.com/sayhellotojungle/cmpunlocker` as a fallback when the upstream
  does not build against a newer NVIDIA driver.
- Docs: removed `--no-gen2-service` from the cmpunlocker examples and tested flags.
- `apply()` now verifies that the **exact fixed top window** (`CMP_TOP_START`,
  384 GiB) is free in the root prefetchable window before touching any GPU or
  bridge. Previously the allocation test accepted any 64 GiB-aligned hole while
  the commit always inserted at the fixed address, so a machine with free
  space elsewhere could pass the dry-run and then fail mid-commit, after the
  GPUs' BARs had already been released and re-sized.
- Kernel compat: the `probe_one()` log that reads
  `pci_host_bridge.preserve_config` / `size_windows` (host-bridge fields present
  since 6.12) is now guarded with `LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)`,
  so the module also builds on pre-6.12 kernels (the line is compiled out there).
- Docs: fix the tested topology (`04:00.0`/`04:10.0`, `08:00.0`/`08:10.0`, and
  the NVMe port behind bus `0a`); rewrite the problem/usage sections of both
  READMEs; add a kernel-compat note and the `mode=0` probe step to
  `docs/INSTALL.md`.
- README: lead with the core problem - on PCIe-switch topologies BAR1 is
  enumerated at 64 MiB, the ReBAR resize fails because the bridge window tree
  cannot grow, and a driver that requires the large BAR hangs at probe - and
  the rationale for a shim over a kernel patch (too heavy to maintain;
  impossible on prebuilt-trimmed-kernel systems such as fnOS).
- Docs: explicitly mark every machine-specific value (addresses, bus numbers,
  log samples, cmpunlocker flags, PCFG offsets, top-window placement) as an
  example from the tested machine, not a standard; de-brand distro-specific
  wording.

## 0.1.1 - 2026-10-30

- Fix cold-boot/initramfs failure: `unlock_rebar()` now writes the ReBAR unlock
  sequence (`CYA=0x30`, `BAR1_CONFIG=0x8000000a`) even when the capability is
  still at the stock `0x400` value. The previous version aborted before
  unlocking when NVIDIA had not run yet, leaving BAR1 at 64 MiB and causing GSP
  initialization to fail.
- Add a 20 ms settle delay after forcing the GPU to D0, for initramfs-time
  access to the PCFG block.
- Validate the initramfs load order (shim before NVIDIA) across a cold reboot.
- Add the `tools/p2p_test.py` real CUDA peer-copy test, `scripts/verify.sh`,
  and log-only `scripts/check_boot_logs.sh`.
- Add install/uninstall scripts, documentation, and license.

## 0.1.0

- Initial runtime shim: manual bridge-window sizing, 384 GiB top window, 64 GiB
  BAR1 / 32 MiB BAR3 per CMP 170HX, PCFG ReBAR unlock, dry-run and resource-test
  modes.
