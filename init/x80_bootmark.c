// SPDX-License-Identifier: GPL-2.0
/*
 * Opt-in PD2183 positive-control experiment, NOT a proven persistent backend.
 * 1) Append three bounded tokens to the existing bootloader-owned PL/LK log
 *    (each initcall stage, persistence proven on #8).
 * 2) Mirror kernel console output into a 16KiB ring placed just after the
 *    PL/LK tail; at late-initcall extend sz_lk once so the next LK save
 *    persists the ring with the actual last kernel messages.
 * Never touch pstore, AEE reboot metadata, storage, or existing log bytes.
 * An absent token/ring cannot establish that an initcall was not reached.
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/string.h>
#include <linux/spinlock.h>
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
/* Mirror ring: after the tail plus 4KiB margin, cap 16KiB. */
#define BM_RING_MARGIN 0x1000
#define BM_RING_CAP 0x4000
#define BM_RING_HDRSZ 16
#define BM_RING_MAGIC 0x4d585830u /* 'X80M' LE */
#define BM_RING_VER 1u

static void __iomem *bm_sram;
static void *bm_log;
static u32 bm_expected_lk;
static u32 bm_expected_off_lk;
static u32 bm_budget;
static u32 bm_ring_base;
static DEFINE_RAW_SPINLOCK(bm_console_lock);

static void x80_bm_console_write(struct console *co, const char *s,
				 unsigned int count)
{
	u32 *hdr;
	u32 off, part;
	unsigned long irqflags;
	void *ring;

	if (!bm_log || !count || count > BM_RING_CAP)
		return;
	/* Panic/NMI recursion must drop output, never wait on its own writer. */
	local_irq_save(irqflags);
	if (!raw_spin_trylock(&bm_console_lock)) {
		local_irq_restore(irqflags);
		return;
	}
	hdr = bm_log + bm_ring_base;
	if (hdr[0] != BM_RING_MAGIC || (hdr[1] & 0xffffu) != BM_RING_CAP ||
	    (hdr[1] >> 16) != BM_RING_VER)
		goto unlock;
	ring = bm_log + bm_ring_base + BM_RING_HDRSZ;
	if (hdr[2] > BM_RING_CAP)
		goto unlock;
	off = hdr[2];
	if (off == BM_RING_CAP) {
		off = 0;
		hdr[3] += 1;
	}
	if (off + count <= BM_RING_CAP) {
		memcpy(ring + off, s, count);
		hdr[2] = off + count;
		__flush_dcache_area(ring + off, count);
	} else {
		part = BM_RING_CAP - off;
		memcpy(ring + off, s, part);
		memcpy(ring, s + part, count - part);
		hdr[3] += 1;
		hdr[2] = count - part;
		if (part)
			__flush_dcache_area(ring + off, part);
		__flush_dcache_area(ring, count - part);
	}
	__flush_dcache_area(hdr, BM_RING_HDRSZ);
	dsb(sy);
unlock:
	raw_spin_unlock(&bm_console_lock);
	local_irq_restore(irqflags);
}

static struct console x80_bm_con = {
	.name = "x80bm",
	.write = x80_bm_console_write,
	.flags = CON_ENABLED | CON_PRINTBUFFER,
	.index = -1,
};

static int __init x80_bm_append(const char *stage)
{
	char text[192];
	u32 start, size, flags;
	int len;

	if (!bm_sram || !bm_log)
		return 0;
	size = readl(bm_sram + BM_SZ_LK);
	start = readl(bm_sram + BM_OFF_LK);
	flags = readl(bm_sram + BM_FLAG);
	/* Abort if another writer changed the length; never overwrite it. */
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
	    ALIGN(start + size + len, 4) > BM_LOG_SIZE ||
	    (bm_ring_base && start + size + len > bm_ring_base))
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
	u32 *hdr;
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
	ret = x80_bm_append("early-initcall");
	if (ret)
		goto fail_unmap;
	/* Adaptive base reserves the remaining token budget and a margin.
	 * Saved length remains bounded by the verified buffer capacity;
	 * acceptance of a larger record by PL remains an experiment.
	 */
	bm_ring_base = ALIGN(off_lk + bm_expected_lk + bm_budget +
			     BM_RING_MARGIN, 0x1000);
	if (bm_ring_base < BM_RING_HDRSZ + BM_RING_CAP ||
	    bm_ring_base + BM_RING_HDRSZ + BM_RING_CAP > BM_LOG_SIZE) {
		ret = -ENOSPC;
		goto fail_unmap;
	}
	hdr = bm_log + bm_ring_base;
	hdr[0] = BM_RING_MAGIC;
	hdr[1] = (BM_RING_VER << 16) | BM_RING_CAP;
	hdr[2] = 0;
	hdr[3] = 0;
	__flush_dcache_area(hdr, BM_RING_HDRSZ);
	dsb(sy);
	pr_notice("%s mirror ring at 0x%x cap 0x%x\n", BM_TOKEN,
		  bm_ring_base, BM_RING_CAP);
	register_console(&x80_bm_con);
	return 0;
fail_unmap:
	memunmap(bm_log);
	bm_log = NULL;
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
	u32 start, size, flags, ext;
	int ret;

	ret = x80_bm_append("late-initcall");
	if (!bm_sram || !bm_log || !bm_ring_base)
		return ret;
	start = readl(bm_sram + BM_OFF_LK);
	size = readl(bm_sram + BM_SZ_LK);
	flags = readl(bm_sram + BM_FLAG);
	ext = bm_ring_base + BM_RING_HDRSZ + BM_RING_CAP - start;
	if (readl(bm_sram) != 0xabcd1234 ||
	    readl(bm_sram + 12) != 0x5678ef90 ||
	    readl(bm_sram + BM_LK_HDR) != 0xcdab3412 ||
	    readl(bm_sram + BM_BUF_ADDR) != BM_LOG_PA ||
	    readl(bm_sram + BM_BUF_SIZE) != BM_LOG_SIZE ||
	    readl(bm_sram + BM_LK_HDR + 4) != BM_LOG_SIZE ||
	    !(flags & 1) || (flags & 0x1d8) ||
	    start != bm_expected_off_lk || size != bm_expected_lk ||
	    start > BM_LOG_SIZE || ext > BM_LOG_SIZE - start ||
	    size > ext || ALIGN(start + ext, 4) > BM_LOG_SIZE)
		return -EINVAL;
	/* Publish the whole buffer before extending the saved length. */
	__flush_dcache_area(bm_log, BM_LOG_SIZE);
	dsb(sy);
	writel(ext, bm_sram + BM_SZ_LK);
	dsb(sy);
	flags = readl(bm_sram + BM_FLAG);
	writel(flags | BM_NEED_SAVE, bm_sram + BM_FLAG);
	dsb(sy);
	pr_notice("%s extended sz_lk 0x%x -> 0x%x (ring persisted if LK saves)\n",
		  BM_TOKEN, size, ext);
	return 0;
}
late_initcall(x80_bm_late);