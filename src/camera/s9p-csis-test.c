// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S9+ (exynos9810): camera bring-up, phase 2a.
 *
 * Captures one frame of the CSIS DMA test pattern into memory, without the
 * sensor and without Samsung's ISP: CSIS1 (v5.1, 0x16240000) configured for
 * RAW10 on two lanes, its VC0 write DMA (0x162c0000) pointed at a buffer
 * below 4 GiB, and the pattern generator of the common CSIS DMA
 * (0x162f0000) feeding it. The frame is read back through debugfs:
 *
 *   /sys/kernel/debug/s9p-csis/frame   the buffer (1 MiB, 0xa5 where unwritten)
 *   /sys/kernel/debug/s9p-csis/regs    CSIS1, DMA, common DMA, SysMMU state
 *   /sys/kernel/debug/s9p-csis/start   write anything: capture again
 *
 * Values from camaras/analisis/03-fase2-csis-dma.md (vendor
 * fimc-is-hw-csi-v5_1.c and the vendor DT). The ISPPRE domain must already
 * be on (it is, from the bootloader); this module never powers it.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>

static unsigned int width = 800, height = 600;
module_param(width, uint, 0444);
module_param(height, uint, 0444);
static bool pack12 = true;
module_param(pack12, bool, 0644);
MODULE_PARM_DESC(pack12, "DMA0_FMT.PACK12 (RAW10 packed); 0: 16 bit per pixel");

#define CSIS1		0x16240000
#define DMA_VC0		0x162c0000
#define DMA_CMN		0x162c0400
#define DMA_COMMON	0x162f0000
#define SYSMMU		0x16310000
#define BUF_SIZE	SZ_1M

static void __iomem *csis, *dma, *cmn, *common, *mmu;
static struct platform_device *pdev;
static void *buf;
static dma_addr_t buf_pa;
static struct debugfs_blob_wrapper blob;
static struct dentry *dir;
static u32 last_src;

static void rmw(void __iomem *r, u32 clr, u32 set)
{
	writel((readl(r) & ~clr) | set, r);
}

static void clocks(void)
{
	void __iomem *top = ioremap(0x1a241000, 0x1000);

	/* CMU_TOP: ISPPRE bus from PLL_SHARED2/2 (400 MHz) divided by 2 */
	if (top) {
		writel(1, top + 0x890);
		iounmap(top);
	}
}

static int capture(void)
{
	u32 w = width, h = height, vblank, s = 0;
	int i, ret;

	memset(buf, 0xa5, BUF_SIZE);

	/* CSIS1 link: reset, 2 lanes, VC/DT interleave, VC0 RAW10 */
	rmw(csis + 0x04, 0, BIT(1));
	readl_poll_timeout(csis + 0x04, s, !(s & BIT(1)), 10, 1000);
	rmw(csis + 0x04, 3 << 8, 1 << 8);
	rmw(csis + 0x04, 3 << 10, 3 << 10);
	writel(0x10ac, csis + 0x40);
	writel(h << 16 | w, csis + 0x44);
	writel(0x011fff1f, csis + 0x10);
	writel(0x0000fff0, csis + 0x18);

	/* VC0 write DMA, OTF input */
	writel(pack12 ? 0x8 : 0x0, dma + 0x04);
	writel(w, dma + 0x40);
	writel(0, cmn + 0x00);
	writel(0, cmn + 0x1c);
	writel(~0, cmn + 0x08);
	writel(0x00f11f00, cmn + 0x04);
	for (i = 0; i < 8; i++)
		writel(lower_32_bits(buf_pa), dma + 0x10 + 4 * i);
	writel(0x3, dma + 0x00);

	/* common DMA and its test pattern, 30 fps at a 200 MHz bus, 2 ppc */
	rmw(common + 0x00, 0xf, 0x6);
	rmw(common + 0x08, 0, BIT(1));
	writel(0x01400140, common + 0x0c);
	writel(0, common + 0x10);
	vblank = (u32)(200000000ull * 2 / 30) - (w + 8) * h - 18;
	writel(0x00100802, common + 0x24);
	writel(h << 16 | w, common + 0x28);
	writel(BIT(30) | (vblank & GENMASK(29, 0)), common + 0x20);
	writel(BIT(31) | BIT(30) | (vblank & GENMASK(29, 0)), common + 0x20);

	ret = readl_poll_timeout(cmn + 0x08, s, s & BIT(8), 1000, 1000000);
	writel(0, dma + 0x00);
	rmw(common + 0x20, BIT(31), 0);
	last_src = s;
	writel(s, cmn + 0x08);
	pr_info("s9p-csis: %s, DMA int src %08x act %08x frames %08x mmu int %08x\n",
		ret ? "no frame end" : "frame captured", s, readl(dma + 0x30),
		readl(csis + 0x100), readl(mmu + 0x60));
	return ret;
}

static int regs_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "buffer %pad, last int src %08x\n", &buf_pa, last_src);
	for (i = 0; i < 0x120; i += 16)
		seq_printf(m, "csis1 +%03x: %08x %08x %08x %08x\n", i, readl(csis + i),
			   readl(csis + i + 4), readl(csis + i + 8), readl(csis + i + 12));
	for (i = 0; i < 0x60; i += 16)
		seq_printf(m, "dma0  +%03x: %08x %08x %08x %08x\n", i, readl(dma + i),
			   readl(dma + i + 4), readl(dma + i + 8), readl(dma + i + 12));
	for (i = 0; i < 0x20; i += 16)
		seq_printf(m, "cmn   +%03x: %08x %08x %08x %08x\n", i, readl(cmn + i),
			   readl(cmn + i + 4), readl(cmn + i + 8), readl(cmn + i + 12));
	for (i = 0; i < 0x30; i += 16)
		seq_printf(m, "common+%03x: %08x %08x %08x %08x\n", i, readl(common + i),
			   readl(common + i + 4), readl(common + i + 8), readl(common + i + 12));
	seq_printf(m, "sysmmu ctrl %08x cfg %08x status %08x int %08x fault va %08x\n",
		   readl(mmu), readl(mmu + 4), readl(mmu + 8), readl(mmu + 0x60), readl(mmu + 0x70));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

static ssize_t start_write(struct file *f, const char __user *u, size_t n, loff_t *pos)
{
	capture();
	return n;
}

static const struct file_operations start_fops = {
	.write = start_write,
};

static int __init s9p_csis_test_init(void)
{
	void __iomem *pmu = ioremap(0x14064000, 0x100);
	u32 st = pmu ? readl(pmu + 0x4c) : 0;

	if (pmu)
		iounmap(pmu);
	if ((st & 0xf) != 0xf) {
		pr_err("s9p-csis: ISPPRE domain is off (%08x): not touching it\n", st);
		return -ENODEV;
	}
	csis = ioremap(CSIS1, 0x1000);
	dma = ioremap(DMA_VC0, 0x400);
	cmn = ioremap(DMA_CMN, 0x100);
	common = ioremap(DMA_COMMON, 0x100);
	mmu = ioremap(SYSMMU, 0x100);
	if (!csis || !dma || !cmn || !common || !mmu)
		goto err_map;
	if (readl(mmu) & BIT(0)) {
		pr_err("s9p-csis: ISPPRE SysMMU is enabled (%08x): not using physical addresses\n",
		       readl(mmu));
		goto err_map;
	}
	pdev = platform_device_register_simple("s9p-csis-test", -1, NULL, 0);
	if (IS_ERR(pdev))
		goto err_map;
	dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	buf = dma_alloc_coherent(&pdev->dev, BUF_SIZE, &buf_pa, GFP_KERNEL);
	if (!buf)
		goto err_dev;
	clocks();
	pr_info("s9p-csis: CSIS1 %08x, buffer at %pad\n", readl(csis), &buf_pa);

	blob.data = buf;
	blob.size = BUF_SIZE;
	dir = debugfs_create_dir("s9p-csis", NULL);
	debugfs_create_blob("frame", 0400, dir, &blob);
	debugfs_create_file("regs", 0400, dir, NULL, &regs_fops);
	debugfs_create_file("start", 0200, dir, NULL, &start_fops);
	capture();
	return 0;

err_dev:
	platform_device_unregister(pdev);
err_map:
	if (csis) iounmap(csis);
	if (dma) iounmap(dma);
	if (cmn) iounmap(cmn);
	if (common) iounmap(common);
	if (mmu) iounmap(mmu);
	return -ENODEV;
}

static void __exit s9p_csis_test_exit(void)
{
	writel(0, dma + 0x00);
	rmw(common + 0x20, BIT(31), 0);
	writel(0, cmn + 0x04);
	debugfs_remove_recursive(dir);
	dma_free_coherent(&pdev->dev, BUF_SIZE, buf, buf_pa);
	platform_device_unregister(pdev);
	iounmap(csis);
	iounmap(dma);
	iounmap(cmn);
	iounmap(common);
	iounmap(mmu);
}

module_init(s9p_csis_test_init);
module_exit(s9p_csis_test_exit);
MODULE_DESCRIPTION("Galaxy S9+ camera bring-up: CSIS1 DMA test pattern capture");
MODULE_LICENSE("GPL");
