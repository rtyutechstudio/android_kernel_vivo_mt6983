// SPDX-License-Identifier: GPL-2.0
/*
 * Opt-in PD2183 positive-control experiment, NOT a proven persistent backend.
 * Append three bounded tokens to the existing bootloader-owned PL/LK log.
 * Never touch pstore, AEE reboot metadata, storage, or existing log bytes.
 * An absent token cannot establish that its initcall was not reached.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/string.h>
#include <generated/utsrelease.h>
#include <asm/cacheflush.h>
#include <asm/unaligned.h>

#define BM_SRAM_PA 0x11df00
#define BM_SRAM_SIZE 0x100
#define BM_LOG_PA 0x7ffbf000
#define BM_LOG_SIZE 0x40000
#define BM_TOKEN "X80BM-v1-20261030-a91c"
/* Offsets are the verified native-LE bootloader ABI, not DT BE cells. */
#define BM_FLAG 16
#define BM_BUF_ADDR 20
#define BM_BUF_SIZE 24
#define BM_LK_HDR 52
#define BM_OFF_PL (BM_LK_HDR + 8)
#define BM_SZ_PL (BM_LK_HDR + 12)
#define BM_OFF_LK (BM_LK_HDR + 20)
#define BM_SZ_LK (BM_LK_HDR + 24)
#define BM_NEED_SAVE 0x4
#define BM_RESERVE 1024

static void __iomem *bm_sram;
static void *bm_log;
static u32 bm_expected_lk;
static u32 bm_expected_off_lk;
static u32 bm_budget;

static int __init x80_bm_append(const char *stage)
{
	char text[192];
	u32 start, size, flags;
	int len;

	if (!bm_sram || !bm_log)
		return 0;
	size = readl(bm_sram + BM_SZ_LK);
	start = readl(bm_sram + BM_OFF_LK);
	/* Abort if another writer changed the length; never overwrite it. */
	flags = readl(bm_sram + BM_FLAG);
	if (readl(bm_sram) != 0xabcd1234 ||
	    readl(bm_sram + 12) != 0x5678ef90 ||
	    readl(bm_sram + BM_LK_HDR) != 0xcdab3412 ||
	    readl(bm_sram + BM_BUF_ADDR) != BM_LOG_PA ||
	    readl(bm_sram + BM_BUF_SIZE) != BM_LOG_SIZE ||
	    readl(bm_sram + BM_LK_HDR + 4) != BM_LOG_SIZE ||
	    !(flags & 1) || (flags & 0x1d8) ||
	    start != bm_expected_off_lk || size != bm_expected_lk ||
	    start > BM_LOG_SIZE || size > BM_LOG_SIZE - start)
		return -EINVAL;
	len = snprintf(text, sizeof(text), "\n%s stage=%s release=%s\n",
		       BM_TOKEN, stage, UTS_RELEASE);
	if (len < 0 || len >= sizeof(text) ||
	    len > bm_budget || len > BM_LOG_SIZE - start - size ||
	    ALIGN(start + size + len, 4) > BM_LOG_SIZE)
		return -ENOSPC;
	memcpy(bm_log + start + size, text, len);
	/* Normal reserved RAM: WB mapping, explicitly publish data to PoC. */
	__flush_dcache_area(bm_log + start + size, len);
	dsb(sy);
	writel(size + len, bm_sram + BM_SZ_LK);
	dsb(sy);
	flags = readl(bm_sram + BM_FLAG);
	writel(flags | BM_NEED_SAVE, bm_sram + BM_FLAG);
	dsb(sy);
	bm_expected_lk += len;
	bm_budget -= len;
	pr_notice("%s stage=%s appended %d bytes (retention unproven)\n",
		  BM_TOKEN, stage, len);
	return 0;
}

static int __init x80_bm_early(void)
{
	struct device_node *np;
	struct resource res;
	const void *tuple;
	u32 off_pl, sz_pl, off_lk, sz_lk, flags;
	int len, ret;

	/* Board guard independent of the opt-in Kconfig. */
	np = of_find_node_by_path("/sound");
	if (!np)
		return -ENODEV;
	{
		const char *model;

		ret = of_property_read_string(np, "vivo,product-model", &model);
		if (!ret && strcmp(model, "PD2183"))
			ret = -ENODEV;
	}
	of_node_put(np);
	if (ret)
		return ret;
	np = of_find_node_by_path("/chosen");
	if (!np)
		return -ENODEV;
	tuple = of_get_property(np, "log_store", &len);
	ret = !tuple || len != 8 ||
		get_unaligned_le32(tuple) != BM_SRAM_PA ||
		get_unaligned_le32(tuple + 4) != BM_SRAM_SIZE;
	of_node_put(np);
	if (ret)
		return -ENODEV;
	np = of_find_compatible_node(NULL, NULL, "mediatek,log_store");
	if (!np)
		return -ENODEV;
	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret || res.start != BM_LOG_PA || resource_size(&res) != BM_LOG_SIZE)
		return -ENODEV;
	bm_sram = ioremap_wc(BM_SRAM_PA, BM_SRAM_SIZE);
	if (!bm_sram)
		return -ENOMEM;
	flags = readl(bm_sram + BM_FLAG);
	off_pl = readl(bm_sram + BM_OFF_PL);
	sz_pl = readl(bm_sram + BM_SZ_PL);
	off_lk = readl(bm_sram + BM_OFF_LK);
	sz_lk = readl(bm_sram + BM_SZ_LK);
	if (readl(bm_sram) != 0xabcd1234 ||
	    readl(bm_sram + 12) != 0x5678ef90 ||
	    readl(bm_sram + BM_LK_HDR) != 0xcdab3412 ||
	    readl(bm_sram + BM_BUF_ADDR) != BM_LOG_PA ||
	    readl(bm_sram + BM_BUF_SIZE) != BM_LOG_SIZE ||
	    readl(bm_sram + BM_LK_HDR + 4) != BM_LOG_SIZE ||
	    !(flags & 1) || (flags & 0x1d8) ||
	    readl(bm_sram + 28) != 0x20 ||
	    readl(bm_sram + 32) > BM_LOG_SIZE ||
	    off_pl < 32 || off_pl > BM_LOG_SIZE ||
	    sz_pl > BM_LOG_SIZE - off_pl ||
	    off_lk != off_pl + sz_pl || off_lk > BM_LOG_SIZE ||
	    sz_lk > BM_LOG_SIZE - off_lk ||
	    BM_LOG_SIZE - off_lk - sz_lk < BM_RESERVE) {
		ret = -EINVAL;
		goto fail;
	}
	bm_log = memremap(BM_LOG_PA, BM_LOG_SIZE, MEMREMAP_WB);
	if (!bm_log) {
		ret = -ENOMEM;
		goto fail;
	}
	bm_expected_lk = sz_lk;
	bm_expected_off_lk = off_lk;
	bm_budget = BM_RESERVE;
	return x80_bm_append("early-initcall");
fail:
	iounmap(bm_sram);
	bm_sram = NULL;
	return ret;
}
early_initcall(x80_bm_early);

static int __init x80_bm_core(void)
{
	return x80_bm_append("core-initcall");
}
core_initcall(x80_bm_core);

static int __init x80_bm_late(void)
{
	int ret = x80_bm_append("late-initcall");

	if (bm_log)
		memunmap(bm_log);
	if (bm_sram)
		iounmap(bm_sram);
	bm_log = NULL;
	bm_sram = NULL;
	return ret;
}
late_initcall(x80_bm_late);
