// SPDX-License-Identifier: GPL-2.0
/*
 * cmp_rebar_shim - re-layout CMP 170HX BAR1 at runtime without touching the
 * kernel image or any distro prebuilt modules.
 *
 * Tested on 3x CMP 170HX behind a PEX88096; see docs/TESTED-CONFIG.md.
 * All sizes and register offsets are examples validated on that card/tree.
 * This module rewrites PCI windows and unlocks vendor registers; use at your
 * own risk.
 *
 * mode=0 (default): read-only probe. Changes nothing.
 * mode=2:           in-memory resource test only. Allocates a dummy window
 *                   from the root bus and releases it; no hardware access.
 * mode=1:           apply. Dry-run unless commit=1 is also given.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/delay.h>

#define DRV_NAME "cmp_rebar_shim"

/*
 * PCFG register layout validated on the CMP 170HX (TU102). These are
 * example values for this card; verify them against your own hardware
 * before adapting the module.
 */
#define CMP_PCFG_OFFSET		0x88000
#define CMP_PCFG_LEN		0x1000
#define CMP_CYA_RESIZE_BAR1	0x724
#define CMP_BAR1_CONFIG		0xDCC
#define CMP_REBAR_CAP_BAR1	0xBBC
#define CMP_REBAR_CTRL_BAR1	0xBC0

static int mode = 0;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "0=probe, 2=resource-test, 1=apply (dry-run unless commit=1)");

static bool read_mmio = true;
module_param(read_mmio, bool, 0444);
MODULE_PARM_DESC(read_mmio, "probe: read CYA/CAP registers through BAR0");

static bool is_cmp(const struct pci_dev *dev)
{
	return dev->vendor == PCI_VENDOR_ID_NVIDIA &&
	       (dev->device == 0x20c2 || dev->device == 0x2082);
}

static struct pci_dev *first_cmp(void)
{
	struct pci_dev *dev;

	dev = pci_get_device(PCI_VENDOR_ID_NVIDIA, 0x20c2, NULL);
	if (dev)
		return dev;
	return pci_get_device(PCI_VENDOR_ID_NVIDIA, 0x2082, NULL);
}

static void dump_bridge_chain(struct pci_dev *dev)
{
	struct pci_bus *bus;
	int i;

	for (bus = dev->bus; bus; bus = bus->parent) {
		if (!bus->self)
			continue;

		pr_info(DRV_NAME ":   bridge %s (bus %02x -> %02x)\n",
			pci_name(bus->self), bus->number,
			bus->self->subordinate ? bus->self->subordinate->number : 0);

		for (i = 0; i < PCI_BRIDGE_RESOURCE_NUM; i++) {
			struct resource *r = bus->resource[i];

			if (!r)
				continue;

			pr_info(DRV_NAME ":     win[%d] %#llx-%#llx size=%#llx flags=%#lx%s%s%s\n",
				i,
				(unsigned long long)r->start,
				(unsigned long long)r->end,
				(unsigned long long)resource_size(r),
				r->flags,
				(r->flags & IORESOURCE_UNSET) ? " UNSET" : "",
				(r->flags & IORESOURCE_SIZEALIGN) ? " SIZEALIGN" : "",
				(r->flags & IORESOURCE_STARTALIGN) ? " STARTALIGN" : "");
		}
	}
}

static void probe_one(struct pci_dev *dev)
{
	struct pci_host_bridge * __maybe_unused host =
		pci_find_host_bridge(dev->bus);
	u32 sizes = pci_rebar_get_possible_sizes(dev, 1);
	int ecap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_REBAR);
	u16 ctrl = 0, cmd = 0;

	pr_info(DRV_NAME ": probe %s vendor=%04x device=%04x\n",
		pci_name(dev), dev->vendor, dev->device);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	/* pci_host_bridge.preserve_config / size_windows exist since 6.12;
	 * compiled out on older kernels (the apply path never uses them).
	 */
	pr_info(DRV_NAME ":   host preserve_config=%u size_windows=%u\n",
		host ? host->preserve_config : 0,
		host ? host->size_windows : 0);
#endif

	pr_info(DRV_NAME ":   BAR0 start=%#llx len=%#llx flags=%#lx\n",
		(unsigned long long)pci_resource_start(dev, 0),
		(unsigned long long)pci_resource_len(dev, 0),
		pci_resource_flags(dev, 0));
	pr_info(DRV_NAME ":   BAR1 start=%#llx len=%#llx flags=%#lx\n",
		(unsigned long long)pci_resource_start(dev, 1),
		(unsigned long long)pci_resource_len(dev, 1),
		pci_resource_flags(dev, 1));
	pr_info(DRV_NAME ":   BAR3 start=%#llx len=%#llx flags=%#lx\n",
		(unsigned long long)pci_resource_start(dev, 3),
		(unsigned long long)pci_resource_len(dev, 3),
		pci_resource_flags(dev, 3));

	pr_info(DRV_NAME ":   REBAR possible sizes mask=%#x (bit N = 2^N MB)\n",
		sizes);

	if (ecap) {
		pci_read_config_word(dev, ecap + PCI_REBAR_CTRL, &ctrl);
		pr_info(DRV_NAME ":   REBAR ext@%#x ctrl=%#x selected=%u (log2 MB)\n",
			ecap, ctrl,
			(ctrl & PCI_REBAR_CTRL_BAR_SIZE) >>
				PCI_REBAR_CTRL_BAR_SHIFT);
	} else {
		pr_info(DRV_NAME ":   no REBAR extended capability\n");
	}

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	pr_info(DRV_NAME ":   PCI_COMMAND=%#x (memory decode %s)\n",
		cmd, (cmd & PCI_COMMAND_MEMORY) ? "on" : "off");

	if (read_mmio && pci_resource_len(dev, 0)) {
		void __iomem *pcfg;

		pcfg = ioremap(pci_resource_start(dev, 0) + CMP_PCFG_OFFSET,
			       CMP_PCFG_LEN);
		if (pcfg) {
			u32 cya = readl(pcfg + CMP_CYA_RESIZE_BAR1);
			u32 cfg = readl(pcfg + CMP_BAR1_CONFIG);
			u32 capv = readl(pcfg + CMP_REBAR_CAP_BAR1);

			pr_info(DRV_NAME ":   MMIO CYA=%#x CFG=%#x CAP=%#x%s\n",
				cya, cfg, capv,
				capv == 0xffffffff ?
					" (window not responding)" : "");
			iounmap(pcfg);
		} else {
			pr_info(DRV_NAME ":   ioremap BAR0+%#x failed\n",
				CMP_PCFG_OFFSET);
		}
	}

	dump_bridge_chain(dev);
}

static int resource_test(void)
{
	struct pci_dev *dev = first_cmp();
	struct pci_bus *root;
	resource_size_t sizes[] = { 384ULL * SZ_1G, 448ULL * SZ_1G, 512ULL * SZ_1G };
	resource_size_t aligns[] = { SZ_64G, SZ_1G };
	int i, j;

	if (!dev) {
		pr_err(DRV_NAME ": no CMP device found\n");
		return -ENODEV;
	}

	root = dev->bus;
	while (root->parent)
		root = root->parent;

	pr_info(DRV_NAME ": resource-test on root bus %02x\n", root->number);

	{
		struct resource *r;
		int i;

		for (i = 0; i < ARRAY_SIZE(root->resource); i++) {
			r = root->resource[i];
			if (!r)
				continue;
			pr_info(DRV_NAME ":   root->resource[%d] %#llx-%#llx size=%#llx flags=%#lx\n",
				i, (unsigned long long)r->start,
				(unsigned long long)r->end,
				(unsigned long long)resource_size(r), r->flags);
		}
		pr_info(DRV_NAME ":   iomem_resource %#llx-%#llx\n",
			(unsigned long long)iomem_resource.start,
			(unsigned long long)iomem_resource.end);
	}

	{
		struct pci_bus *b = dev->bus;
		struct resource *win, *root_win;

		while (b->parent && b->parent->parent)
			b = b->parent;
		win = b->resource[2];
		root_win = win ? win->parent : NULL;
		pr_info(DRV_NAME ":   top bridge %s win %#llx-%#llx parent %#llx-%#llx\n",
			b->self ? pci_name(b->self) : "?",
			win ? (unsigned long long)win->start : 0,
			win ? (unsigned long long)win->end : 0,
			root_win ? (unsigned long long)root_win->start : 0,
			root_win ? (unsigned long long)root_win->end : 0);

		for (i = 0; root_win && i < ARRAY_SIZE(sizes); i++) {
			for (j = 0; j < ARRAY_SIZE(aligns); j++) {
				struct resource *r = kzalloc(sizeof(*r), GFP_KERNEL);
				int ret;

				if (!r) {
					pci_dev_put(dev);
					return -ENOMEM;
				}
				r->flags = IORESOURCE_MEM;
				ret = allocate_resource(root_win, r, sizes[i],
							0, ~0ULL, aligns[j], NULL, NULL);
				pr_info(DRV_NAME ":   rootwin alloc %lluG align %lluG -> %d (%#llx-%#llx)\n",
					(unsigned long long)(sizes[i] / SZ_1G),
					(unsigned long long)(aligns[j] / SZ_1G),
					ret, (unsigned long long)r->start,
					(unsigned long long)r->end);
				if (!ret)
					release_resource(r);
				kfree(r);
			}
		}
	}

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		for (j = 0; j < ARRAY_SIZE(aligns); j++) {
			struct resource *r;
			int ret;

			r = kzalloc(sizeof(*r), GFP_KERNEL);
			if (!r) {
				pci_dev_put(dev);
				return -ENOMEM;
			}
			r->flags = IORESOURCE_MEM;

			ret = allocate_resource(&iomem_resource, r, sizes[i],
						0, ~0ULL, aligns[j], NULL, NULL);
			pr_info(DRV_NAME ":   iomem alloc %lluG align %lluG -> %d (%#llx-%#llx)\n",
				(unsigned long long)(sizes[i] / SZ_1G),
				(unsigned long long)(aligns[j] / SZ_1G),
				ret,
				(unsigned long long)r->start,
				(unsigned long long)r->end);
			if (!ret)
				release_resource(r);
			kfree(r);
		}
	}

	pci_dev_put(dev);
	return 0;
}

/* ---------------- apply ---------------- */

/* Fixed start of the top window; the allocation test below verifies the
 * exact range CMP_TOP_START..CMP_TOP_START+top_size-1 is free before the
 * commit inserts it there. The 64 GiB start is an example that fits the
 * tested tree; adapt it to your root window layout.
 */
#define CMP_TOP_START	(64ULL << 30)

static bool commit;
module_param(commit, bool, 0444);
MODULE_PARM_DESC(commit, "mode=1: actually rewrite windows (default: dry-run)");

static resource_size_t bus_pref_size(struct pci_bus *bus)
{
	struct pci_dev *dev;
	resource_size_t size = 0;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (dev->subordinate) {
			resource_size_t cs = bus_pref_size(dev->subordinate);

			if (cs)
				size += ALIGN(cs, SZ_64G);
		} else if (is_cmp(dev)) {
			size += ALIGN(64ULL << 30, SZ_64G);
			size += ALIGN(32ULL << 20, 32ULL << 20);
		}
	}
	return size;
}

static struct pci_bus *find_top_bus(struct pci_bus *bus)
{
	while (bus->parent && bus->parent->parent)
		bus = bus->parent;
	return bus;
}

static void dump_plan_walk(struct pci_bus *bus, bool is_top)
{
	struct pci_dev *dev;
	struct resource *win = bus->resource[2];
	resource_size_t size = bus_pref_size(bus);
	resource_size_t start = is_top ? CMP_TOP_START : SZ_64G;

	if (!size || !win)
		return;

	pr_info(DRV_NAME ":   bus %02x%s size=%#llx plan=%#llx-%#llx (cur %#llx-%#llx)\n",
		bus->number, is_top ? " TOP" : "",
		(unsigned long long)size,
		(unsigned long long)start,
		(unsigned long long)(start + size - 1),
		(unsigned long long)win->start,
		(unsigned long long)win->end);

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (dev->subordinate)
			dump_plan_walk(dev->subordinate, false);
	}
}

static void release_walk(struct pci_bus *bus)
{
	struct pci_dev *dev;
	struct resource *win;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (dev->subordinate)
			release_walk(dev->subordinate);
	}

	if (!bus_pref_size(bus))
		return;

	win = bus->resource[2];
	if (win && win->parent)
		release_resource(win);
}

static int set_sizes_walk(struct pci_bus *bus, bool is_top)
{
	struct pci_dev *dev;
	struct resource *win;
	resource_size_t size = bus_pref_size(bus);

	if (!size)
		return 0;

	win = bus->resource[2];
	if (!win)
		return 0;

	if (is_top) {
		win->start = CMP_TOP_START;
		win->end = CMP_TOP_START + size - 1;
		win->flags &= ~(IORESOURCE_UNSET | IORESOURCE_STARTALIGN);
	} else {
		win->start = SZ_64G;		/* alignment */
		win->end = SZ_64G + size - 1;
		win->flags &= ~IORESOURCE_UNSET;
		win->flags |= IORESOURCE_STARTALIGN;
	}
	win->flags |= IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_MEM_64;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (dev->subordinate) {
			int ret = set_sizes_walk(dev->subordinate, false);

			if (ret)
				return ret;
		}
	}
	return 0;
}

static void write_bridges_walk(struct pci_bus *bus)
{
	struct pci_dev *dev;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (!dev->subordinate)
			continue;
		pci_assign_unassigned_bridge_resources(dev);
		write_bridges_walk(dev->subordinate);
	}
}

static int unlock_rebar(struct pci_dev *dev)
{
	void __iomem *pcfg;
	u16 cmd;
	u32 cap, ctrl;
	int ret = 0;

	pcfg = ioremap(pci_resource_start(dev, 0) + CMP_PCFG_OFFSET,
		       CMP_PCFG_LEN);
	if (!pcfg) {
		pr_err(DRV_NAME ": %s: ioremap PCFG failed\n", pci_name(dev));
		return -ENOMEM;
	}

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	if (!(cmd & PCI_COMMAND_MEMORY))
		pci_write_config_word(dev, PCI_COMMAND, cmd | PCI_COMMAND_MEMORY);

	cap = readl(pcfg + CMP_REBAR_CAP_BAR1);
	if (cap == 0xffffffff) {
		pr_err(DRV_NAME ": %s: REBAR cap unreadable (%#x)\n",
		       pci_name(dev), cap);
		ret = -EIO;
		goto out;
	}
	if (!(cap & BIT(20)))
		pr_info(DRV_NAME ": %s: REBAR cap locked (%#x), unlocking\n",
			pci_name(dev), cap);

	writel(0x30, pcfg + CMP_CYA_RESIZE_BAR1);
	writel(0x8000000a, pcfg + CMP_BAR1_CONFIG);
	cap = readl(pcfg + CMP_REBAR_CAP_BAR1);
	if (cap == 0xffffffff || !(cap & BIT(20))) {
		pr_err(DRV_NAME ": %s: unlock did not take (%#x)\n",
		       pci_name(dev), cap);
		ret = -EIO;
		goto out;
	}

	ctrl = readl(pcfg + CMP_REBAR_CTRL_BAR1);
	ctrl &= ~0x3f00;
	ctrl |= 16 << 8;
	writel(ctrl, pcfg + CMP_REBAR_CTRL_BAR1);
	ctrl = readl(pcfg + CMP_REBAR_CTRL_BAR1);
	pr_info(DRV_NAME ": %s: REBAR ctrl now %#x (size=%u)\n",
		pci_name(dev), ctrl, (ctrl >> 8) & 0x3f);
out:
	pci_write_config_word(dev, PCI_COMMAND, cmd);
	iounmap(pcfg);
	return ret;
}

static int prepare_gpu(struct pci_dev *dev, bool do_commit)
{
	struct resource *r1 = &dev->resource[1];
	struct resource *r3 = &dev->resource[3];
	u16 cmd = 0;

	pr_info(DRV_NAME ": GPU %s: BAR1 %#llx/%#llx BAR3 %#llx/%#llx -> 64G/32M\n",
		pci_name(dev),
		(unsigned long long)r1->start,
		(unsigned long long)resource_size(r1),
		(unsigned long long)r3->start,
		(unsigned long long)resource_size(r3));

	if (!do_commit)
		return 0;

	/* The driver may have left the GPU in D3; wake it before touching BAR0. */
	if (pci_set_power_state(dev, PCI_D0))
		pr_warn(DRV_NAME ": %s: could not force D0\n", pci_name(dev));

	msleep(20);

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	if (cmd & PCI_COMMAND_MEMORY)
		pci_write_config_word(dev, PCI_COMMAND,
				      cmd & ~PCI_COMMAND_MEMORY);

	if (unlock_rebar(dev)) {
		pci_write_config_word(dev, PCI_COMMAND, cmd);
		return -EIO;
	}

	pci_release_resource(dev, 1);
	pci_release_resource(dev, 3);

	r1->start = 0;
	r1->end = (64ULL << 30) - 1;
	r1->flags |= IORESOURCE_MEM | IORESOURCE_PREFETCH |
		     IORESOURCE_MEM_64 | IORESOURCE_SIZEALIGN;

	r3->start = 0;
	r3->end = (32ULL << 20) - 1;
	r3->flags |= IORESOURCE_MEM | IORESOURCE_PREFETCH |
		     IORESOURCE_MEM_64 | IORESOURCE_SIZEALIGN;

	/* keep memory decode off until the new windows are live */
	return 0;
}

static void enable_gpu(struct pci_dev *dev)
{
	u16 cmd = 0;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	pci_write_config_word(dev, PCI_COMMAND, cmd | PCI_COMMAND_MEMORY);
}

static int apply(bool do_commit)
{
	struct pci_dev *dev, *gpu;
	struct pci_bus *top_bus;
	struct resource *top_win, *root_win;
	resource_size_t top_size;
	int ret;

	gpu = first_cmp();
	if (!gpu) {
		pr_err(DRV_NAME ": no CMP device\n");
		return -ENODEV;
	}

	top_bus = find_top_bus(gpu->bus);
	top_win = top_bus->resource[2];
	root_win = top_win ? top_win->parent : NULL;

	pr_info(DRV_NAME ": apply commit=%d top bus %02x bridge %s\n",
		do_commit, top_bus->number,
		top_bus->self ? pci_name(top_bus->self) : "?");

	if (!top_win || !root_win) {
		pr_err(DRV_NAME ": top window/root window missing\n");
		pci_dev_put(gpu);
		return -ENODEV;
	}

	top_size = bus_pref_size(top_bus);
	pr_info(DRV_NAME ": root window %#llx-%#llx, top needs %#llx\n",
		(unsigned long long)root_win->start,
		(unsigned long long)root_win->end,
		(unsigned long long)top_size);

	if (top_size != (384ULL << 30))
		pr_warn(DRV_NAME ": unexpected top size %#llx (want 384G)\n",
			(unsigned long long)top_size);

	dump_plan_walk(top_bus, true);

	for_each_pci_dev(dev) {
		if (is_cmp(dev))
			prepare_gpu(dev, false);
	}

	{
		struct resource *probe = kzalloc(sizeof(*probe), GFP_KERNEL);

		if (!probe) {
			pci_dev_put(gpu);
			return -ENOMEM;
		}
		probe->flags = IORESOURCE_MEM;
		ret = allocate_resource(root_win, probe, top_size,
					CMP_TOP_START,
					CMP_TOP_START + top_size - 1, 0,
					NULL, NULL);
		pr_info(DRV_NAME ": top allocation test: ret=%d %#llx-%#llx\n",
			ret, (unsigned long long)probe->start,
			(unsigned long long)probe->end);
		if (!ret)
			release_resource(probe);
		kfree(probe);
		if (ret) {
			pr_err(DRV_NAME ": fixed top window %#llx-%#llx unavailable in root window (ret=%d)\n",
			       (unsigned long long)CMP_TOP_START,
			       (unsigned long long)(CMP_TOP_START + top_size - 1),
			       ret);
			pci_dev_put(gpu);
			return ret;
		}
	}

	if (!do_commit) {
		pr_info(DRV_NAME ": dry-run only (commit=1 to apply)\n");
		pci_dev_put(gpu);
		return 0;
	}

	for_each_pci_dev(dev) {
		if (!is_cmp(dev))
			continue;
		ret = prepare_gpu(dev, true);
		if (ret) {
			pci_dev_put(gpu);
			return ret;
		}
	}

	release_walk(top_bus);

	ret = set_sizes_walk(top_bus, true);
	if (ret) {
		pci_dev_put(gpu);
		return ret;
	}

	ret = request_resource(root_win, top_win);
	pr_info(DRV_NAME ": top insert %#llx-%#llx -> %d\n",
		(unsigned long long)top_win->start,
		(unsigned long long)top_win->end, ret);
	if (ret) {
		pci_dev_put(gpu);
		return ret;
	}

	pci_bus_assign_resources(top_bus);
	pr_info(DRV_NAME ": pci_bus_assign_resources done\n");

	if (top_bus->self)
		pci_assign_unassigned_bridge_resources(top_bus->self);
	write_bridges_walk(top_bus);
	pr_info(DRV_NAME ": bridge registers written\n");

	for_each_pci_dev(dev) {
		if (!is_cmp(dev))
			continue;
		enable_gpu(dev);
		pr_info(DRV_NAME ": verify %s BAR1 %#llx-%#llx len=%#llx flags=%#lx BAR3 %#llx-%#llx len=%#llx\n",
			pci_name(dev),
			(unsigned long long)dev->resource[1].start,
			(unsigned long long)dev->resource[1].end,
			(unsigned long long)resource_size(&dev->resource[1]),
			dev->resource[1].flags,
			(unsigned long long)dev->resource[3].start,
			(unsigned long long)dev->resource[3].end,
			(unsigned long long)resource_size(&dev->resource[3]));
	}

	pci_dev_put(gpu);
	return 0;
}

static int __init cmp_rebar_shim_init(void)
{
	struct pci_dev *dev = NULL;
	int found = 0;

	if (mode == 2)
		return resource_test();

	if (mode == 1)
		return apply(commit);

	if (mode != 0) {
		pr_err(DRV_NAME ": mode=%d not implemented yet\n", mode);
		return -EOPNOTSUPP;
	}

	pr_info(DRV_NAME ": probe mode, read_mmio=%d\n", read_mmio);

	for_each_pci_dev(dev) {
		if (!is_cmp(dev))
			continue;
		probe_one(dev);
		found++;
	}

	pr_info(DRV_NAME ": %d CMP device(s) probed\n", found);
	return 0;
}

static void __exit cmp_rebar_shim_exit(void)
{
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(cmp_rebar_shim_init);
module_exit(cmp_rebar_shim_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("cmp-rebar-shim contributors");
MODULE_DESCRIPTION("Runtime 64G BAR1 window shim for NVIDIA CMP 170HX");
MODULE_VERSION("0.1.2");
