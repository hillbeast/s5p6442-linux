// SPDX-License-Identifier: GPL-2.0
//
// Samsung S5P6442 based machine.
//
// Based on S5PV210 device tree
//
// Copyright (c) 2013-2014 Samsung Electronics Co., Ltd.
// Mateusz Krawczuk <m.krawczuk@partner.samsung.com>
// Tomasz Figa <t.figa@samsung.com>
// Mark Kennard <markkennard4@gmail.com>

#include <linux/of_fdt.h>
#include <linux/platform_device.h>
#include <linux/memblock.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/system_misc.h>

#include "common.h"
#include "s5p6442.h"
#include "regs-clock.h"

static int __init s5pv210_fdt_map_sys(unsigned long node, const char *uname,
					int depth, void *data)
{
	struct map_desc iodesc;
	const __be32 *reg;
	int len;

	if (!of_flat_dt_is_compatible(node, "samsung,s5pv210-clock"))
		return 0;

	reg = of_get_flat_dt_prop(node, "reg", &len);
	if (reg == NULL || len != (sizeof(unsigned long) * 2))
		return 0;

	iodesc.pfn = __phys_to_pfn(be32_to_cpu(reg[0]));
	iodesc.length = be32_to_cpu(reg[1]) - 1;
	iodesc.virtual = (unsigned long)S3C_VA_SYS;
	iodesc.type = MT_DEVICE;
	iotable_init(&iodesc, 1);

	return 1;
}

static void __init s5p6442_dt_map_io(void)
{
	debug_ll_io_init();

	of_scan_flat_dt(s5pv210_fdt_map_sys, NULL);
}

static void s5p6442_dt_restart(enum reboot_mode mode, const char *cmd)
{
	__raw_writel(0x1, S5P_SWRESET);
}

static void __init s5p6442_dt_init_late(void)
{
	platform_device_register_simple("s5pv210-cpufreq", -1, NULL, 0);
	s5pv210_pm_init();
}

static char const *const s5p6442_dt_compat[] __initconst = {
	"samsung,s5p6442",
	NULL
};

void __init s5p6442_fixup(struct tag *t, char **from)
{
	memblock_add(PHYS_OFFSET, APOLLO_PHYS_SIZE_DDR);
}

DT_MACHINE_START(S5P6442_DT, "Samsung S5P6442-based board")
	.fixup = s5p6442_fixup,
	.dt_compat = s5p6442_dt_compat,
	.map_io = s5p6442_dt_map_io,
	.restart = s5p6442_dt_restart,
	.init_late = s5p6442_dt_init_late,
	.nr = APOLLO_MACHID,
MACHINE_END
