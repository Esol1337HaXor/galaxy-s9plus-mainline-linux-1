"""Wire this port's own drivers into the mainline tree's Kbuild files.

Idempotent: runs before every build, does nothing if the entries are already
there. Takes the kernel source directory as its only argument.
"""
import re
import sys
from pathlib import Path

src = Path(sys.argv[1])
TAB = "\t"


def insert_before(path, anchor, text, marker):
    p = src / path
    s = p.read_text()
    if marker in s:
        print(f"{path}: {marker} already there")
        return
    assert anchor in s, f"{path}: anchor {anchor!r} not found"
    p.write_text(s.replace(anchor, text + anchor, 1))
    print(f"{path}: {marker} added")


def insert_after(path, anchor, text, marker):
    p = src / path
    s = p.read_text()
    if marker in s:
        print(f"{path}: {marker} already there")
        return
    if anchor.endswith("\n"):
        line = anchor
    else:
        matches = [l for l in s.splitlines(keepends=True) if anchor in l]
        assert len(matches) == 1, f"{path}: {anchor!r} matched {len(matches)} lines"
        line = matches[0]
    assert line in s, f"{path}: anchor {anchor!r} not found"
    p.write_text(s.replace(line, line + text, 1))
    print(f"{path}: {marker} added")


# --- UFS host driver for the Exynos 9810 -------------------------------------
insert_after(
    "drivers/ufs/host/Makefile",
    "obj-$(CONFIG_SCSI_UFS_EXYNOS) += ufs-exynos.o",
    "obj-$(CONFIG_SCSI_UFS_EXYNOS9810) += ufs-exynos9810.o ufs-cal-9810.o\n",
    "SCSI_UFS_EXYNOS9810",
)

ufs_kconfig = f"""config SCSI_UFS_EXYNOS9810
{TAB}tristate "Exynos 9810 UFS host controller driver"
{TAB}depends on SCSI_UFSHCD_PLATFORM && (ARCH_EXYNOS || COMPILE_TEST)
{TAB}help
{TAB}  UFS host support for the Samsung Exynos 9810, whose PHY and UNIPRO
{TAB}  calibration differs from every variant ufs-exynos.c covers. Uses
{TAB}  Samsung's own ufs-cal-9810 sequence, unchanged from the vendor kernel.

"""
insert_before(
    "drivers/ufs/host/Kconfig",
    "config SCSI_UFS_VARIABLE_SG_ENTRY_SIZE\n",
    ufs_kconfig,
    "SCSI_UFS_EXYNOS9810",
)

# The core only honours hba->sg_entry_size when this is on, and our FMP
# descriptor type needs 128-byte PRDT entries.
insert_after(
    "drivers/ufs/host/Kconfig",
    TAB + "default y if SCSI_UFS_EXYNOS && SCSI_UFS_CRYPTO" + chr(10),
    TAB + "default y if SCSI_UFS_EXYNOS9810" + chr(10),
    "default y if SCSI_UFS_EXYNOS9810",
)

# --- clock gates the port opens by hand (no 9810 clock driver) -------------
insert_after(
    "drivers/soc/samsung/Makefile",
    "obj-$(CONFIG_EXYNOS_PMU)",
    "obj-y\t\t\t\t+= s9p-clkgate.o\n",
    "s9p-clkgate.o",
)

# --- G3D power domain (vendor on/off sequence plus the EL3 TZPC restore) ---
insert_after(
    "drivers/soc/samsung/Makefile",
    "s9p-clkgate.o",
    "obj-y\t\t\t\t+= s9p-g3d-pd.o\n",
    "s9p-g3d-pd.o",
)

# --- MAX77705 Type-C MCU: route D+/D- to the AP -----------------------------
insert_after(
    "drivers/soc/samsung/Makefile",
    "s9p-g3d-pd.o",
    "obj-$(CONFIG_I2C)\t\t+= s9p-max77705-muic.o\n",
    "s9p-max77705-muic.o",
)

# --- USB 2.0 PHY, vendor CAL for this exact PHY revision -------------------
insert_after(
    "drivers/phy/samsung/Makefile",
    "phy-exynos5-usbdrd.o",
    "obj-$(CONFIG_GENERIC_PHY)\t+= phy-exynos9810-usbdrd.o\n",
    "phy-exynos9810-usbdrd.o",
)

# --- the bootloader framebuffer, presented as a KMS device named "exynos" ----
insert_after(
    "drivers/gpu/drm/sysfb/Makefile",
    "simpledrm.o",
    f"obj-$(CONFIG_DRM_EXYNOS_BOOTFB){TAB}+= exynos-bootfb.o\n",
    "DRM_EXYNOS_BOOTFB",
)

fb_kconfig = f"""config DRM_EXYNOS_BOOTFB
{TAB}tristate "Exynos bootloader framebuffer (KMS device named exynos)"
{TAB}depends on DRM && MMU
{TAB}select APERTURE_HELPERS
{TAB}select DRM_CLIENT_SELECTION
{TAB}select DRM_GEM_SHMEM_HELPER
{TAB}select DRM_KMS_HELPER
{TAB}select DRM_SYSFB_HELPER
{TAB}help
{TAB}  simpledrm under a different name. Samsung's DECON keeps scanning out
{TAB}  the framebuffer the bootloader set up, so that buffer works as a
{TAB}  scanout without a display driver -- and calling the device "exynos"
{TAB}  is what makes Mesa pair it with the Panfrost render node.

"""
insert_before(
    "drivers/gpu/drm/sysfb/Kconfig",
    "config DRM_SIMPLEDRM\n",
    fb_kconfig,
    "DRM_EXYNOS_BOOTFB",
)

# --- HSI2C: say what the controller was doing when a transfer timed out -----
# On this SoC every transfer ended in "tx timeout" although the bus is set up
# like the vendor's. Before giving up, poll the interrupt status by hand (so a
# transfer that completed without its interrupt arriving still succeeds, and
# says so), and log the controller's registers otherwise.
i2c_path = "drivers/i2c/busses/i2c-exynos5.c"
i2c = (src / i2c_path).read_text()
if "S9P_I2C_DIAG" not in i2c:
    inc_anchor = "#include <linux/i2c.h>\n"
    assert inc_anchor in i2c
    i2c = i2c.replace(inc_anchor, inc_anchor + "#include <linux/kernel_stat.h> /* S9P_I2C_DIAG */\n", 1)
    old = TAB + "if (time_left == 0)\n" + TAB + TAB + "ret = -ETIMEDOUT;\n"
    assert i2c.count(old) == 1, f"{i2c_path}: timeout anchor matched {i2c.count(old)} times"
    new = (
        TAB + "if (time_left == 0 && !i2c->atomic &&\n"
        + TAB + "    exynos5_i2c_poll_irqs_timeout(i2c, msecs_to_jiffies(20))) {\n"
        + TAB + TAB + "dev_warn_once(i2c->dev, \"S9P: transfer finished but its interrupt (%d, %u so far) never arrived; polled\\n\",\n"
        + TAB + TAB + TAB + "      i2c->irq, kstat_irqs_usr(i2c->irq));\n"
        + TAB + TAB + "time_left = 1;\n"
        + TAB + "}\n"
        + TAB + "if (time_left == 0) {\n"
        + TAB + TAB + "ret = -ETIMEDOUT;\n"
        + TAB + TAB + "dev_err(i2c->dev, \"S9P %s addr=%02x len=%d irq=%d(%u): ctl=%08x fifoctl=%08x inten=%08x intst=%08x fifost=%08x conf=%08x auto=%08x to=%08x trans=%08x fs=%08x/%08x/%08x usi_con=%08x usi_opt=%08x\\n\",\n"
        + TAB + TAB + TAB + "(msgs->flags & I2C_M_RD) ? \"rd\" : \"wr\", msgs->addr, msgs->len,\n"
        + TAB + TAB + TAB + "i2c->irq, kstat_irqs_usr(i2c->irq),\n"
        + TAB + TAB + TAB + "readl(i2c->regs + 0x00), readl(i2c->regs + 0x04),\n"
        + TAB + TAB + TAB + "readl(i2c->regs + 0x20), readl(i2c->regs + 0x24),\n"
        + TAB + TAB + TAB + "readl(i2c->regs + 0x30), readl(i2c->regs + 0x40),\n"
        + TAB + TAB + TAB + "readl(i2c->regs + 0x44), readl(i2c->regs + 0x48),\n"
        + TAB + TAB + TAB + "readl(i2c->regs + 0x50), readl(i2c->regs + 0x60),\n"
        + TAB + TAB + TAB + "readl(i2c->regs + 0x64), readl(i2c->regs + 0x68),\n"
        + TAB + TAB + TAB + "readl(i2c->regs + 0xc4), readl(i2c->regs + 0xc8));\n"
        + TAB + "}\n"
    )
    i2c = i2c.replace(old, new, 1)
    (src / i2c_path).write_text(i2c)
    print(f"{i2c_path}: S9P_I2C_DIAG added")
else:
    print(f"{i2c_path}: S9P_I2C_DIAG already there")

# --- Panfrost: keep GPU buffers where the display block can reach them ------
# DECON addresses scanout with a 32-bit register, and this board has 6 GB:
# ZONE_DMA holds the 1.9 GB below 4 GB, ZONE_NORMAL everything above, and
# DMA32 is empty. A GPU buffer handed to KMS for direct scanout therefore
# lands above 4 GB, and importing it into the DECON device goes through
# swiotlb -- where it cannot fit either, because panfrost backs buffers with
# transparent hugepages and a 2 MB segment exceeds swiotlb's 256 KB maximum
# ("swiotlb buffer is full" in the log, once per attempt). The import fails
# and the compositor falls back to copying every frame.
#
# __GFP_DMA32 on panfrost's shmem mappings puts those pages in ZONE_DMA (the
# zonelist falls through an empty DMA32), so the import is a plain mapping
# and scanout works straight off the rendered buffer. The GPU itself is
# unaffected: its MMU is 40-bit and does not care where the pages are.
gem_c = "drivers/gpu/drm/panfrost/panfrost_gem.c"
g_path = src / gem_c
g = g_path.read_text()
if "S9P_GEM_DMA32" not in g:
    anchor_create = (TAB + "shmem = drm_gem_shmem_create(dev, size);" + chr(10)
                     + TAB + "if (IS_ERR(shmem))" + chr(10)
                     + TAB + TAB + "return ERR_CAST(shmem);" + chr(10))
    assert anchor_create in g, f"{gem_c}: shmem_create anchor not found"
    g = g.replace(anchor_create, anchor_create + chr(10)
        + TAB + "/* S9P_GEM_DMA32: below 4 GB, so KMS can scan this out directly */" + chr(10)
        + TAB + "mapping_set_gfp_mask(shmem->base.filp->f_mapping," + chr(10)
        + TAB + TAB + TAB + "     (mapping_gfp_mask(shmem->base.filp->f_mapping)" + chr(10)
        + TAB + TAB + TAB + "      & ~__GFP_HIGHMEM) | __GFP_DMA32);" + chr(10), 1)
    inc = "#include <linux/err.h>" + chr(10)
    if inc in g:
        g = g.replace(inc, inc + "#include <linux/pagemap.h> /* S9P_GEM_DMA32 */" + chr(10), 1)
    else:
        first = g.index("#include")
        g = g[:first] + "#include <linux/pagemap.h> /* S9P_GEM_DMA32 */" + chr(10) + g[first:]
    g_path.write_text(g)
    print(f"{gem_c}: GPU buffers restricted to the low 4 GB")
else:
    print(f"{gem_c}: S9P_GEM_DMA32 already there")

# --- Panfrost: make the job-slot flush reduction switchable at run time ------
# Mali-G72 r0p1 on this SoC throws sporadic DATA_INVALID_FAULT / page faults at
# VA 0 under allocation churn, which vanish with PAN_MESA_DEBUG=sync. Flush
# reduction lets the GPU skip cache clean/invalidate between jobs; the switch
# (/sys/module/panfrost/parameters/s9p_flush_reduction, or
# panfrost.s9p_flush_reduction=0 on the command line) tests that hypothesis
# without a rebuild.
pj_path = "drivers/gpu/drm/panfrost/panfrost_job.c"
pj = (src / pj_path).read_text()
if "S9P_FLUSH" not in pj:
    old = "cfg |= JS_CONFIG_ENABLE_FLUSH_REDUCTION;"
    assert pj.count(old) == 1, f"{pj_path}: flush-reduction anchor matched {pj.count(old)} times"
    pj = pj.replace(old, "cfg |= s9p_flush_reduction ? JS_CONFIG_ENABLE_FLUSH_REDUCTION : 0; /* S9P_FLUSH */", 1)
    first_static = pj.index("\nstatic ")
    pj = (pj[:first_static]
          + "\n#include <linux/moduleparam.h> /* S9P_FLUSH */\n"
          + "static bool s9p_flush_reduction = true;\n"
          + "module_param_named(s9p_flush_reduction, s9p_flush_reduction, bool, 0644);\n"
          + "MODULE_PARM_DESC(s9p_flush_reduction, \"use JS_CONFIG_ENABLE_FLUSH_REDUCTION on GPUs that offer it (S9P experiment)\");\n"
          + pj[first_static:])
    (src / pj_path).write_text(pj)
    print(f"{pj_path}: S9P_FLUSH added")
else:
    print(f"{pj_path}: S9P_FLUSH already there")

# --- Panfrost: retain submission owner for actionable fault diagnosis ------
# Fault logs previously omitted the client, so a compositor fault after a
# benchmark could be mistaken for a fault in that benchmark. Log only on
# faults; no per-frame printk and no register/clock changes.
insert_before('drivers/gpu/drm/panfrost/panfrost_job.h',
              '#include <uapi/drm/panfrost_drm.h>',
              '#include <linux/sched.h> /* S9P_JOB_OWNER_INCLUDE */\n',
              'S9P_JOB_OWNER_INCLUDE')
insert_after('drivers/gpu/drm/panfrost/panfrost_job.h',
             'struct panfrost_job {\n',
             '\tpid_t submitter_tgid; /* S9P_JOB_OWNER */\n'
             '\tchar submitter_comm[TASK_COMM_LEN];\n',
             'S9P_JOB_OWNER */')
insert_after('drivers/gpu/drm/panfrost/panfrost_drv.c',
             '\tjob->pfdev = pfdev;\n',
             '\tjob->submitter_tgid = task_tgid_nr(current); /* S9P_JOB_OWNER */\n'
             '\tget_task_comm(job->submitter_comm, current);\n',
             'S9P_JOB_OWNER')
pj = (src / pj_path).read_text()
if 'S9P_JOB_OWNER' not in pj:
    anchor = '\t} else {\n\t\tdev_err(pfdev->base.dev, "js fault,'
    assert pj.count(anchor) == 1
    diagnostic = ('\t} else {\n'
        '\t\t/* S9P_JOB_OWNER: record the actual ioctl submitter. */\n'
        '\t\tdev_err(pfdev->base.dev, "job owner=%s tgid=%d bos=%u jc=%#llx\\n",\n'
        '\t\t\tjob->submitter_comm, job->submitter_tgid, job->bo_count, job->jc);\n'
        '\t\tdev_err(pfdev->base.dev, "js fault,')
    (src / pj_path).write_text(pj.replace(anchor, diagnostic, 1))

# --- sysfb: invalidate the CPU cache before blitting a GPU-written FB -------
# Mutter renders with Panfrost straight into the KMS driver's shmem dumb
# buffers (exported as dma-buf). Those are mapped cached on the CPU side
# (map_wc is false for native shmem objects), and the per-frame blit into the
# sboot framebuffer reads them with the CPU, so stale cache lines from the
# previous frame end up on screen. drm_gem_fb_begin_cpu_access() only syncs
# *imported* objects. Map the pages for our own (non-coherent) device once and
# invalidate before every blit; the device needs a DMA mask that covers the
# RAM above 4 GB or the mapping would bounce through swiotlb.
sm_path = "drivers/gpu/drm/sysfb/drm_sysfb_modeset.c"
sm = (src / sm_path).read_text()
if "S9P_FB_SYNC" not in sm:
    inc = '#include "drm_sysfb_helper.h"\n'
    assert inc in sm
    sm = sm.replace(inc, "#include <linux/dma-mapping.h> /* S9P_FB_SYNC */\n#include <drm/drm_gem_shmem_helper.h>\n" + inc, 1)
    old = TAB + "ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);\n"
    assert sm.count(old) == 1, f"{sm_path}: begin_cpu_access anchor matched {sm.count(old)} times"
    new = (TAB + "if (fb->obj[0] && !drm_gem_is_imported(fb->obj[0])) { /* S9P_FB_SYNC */\n"
           + TAB + TAB + "struct sg_table *s9p_sgt = drm_gem_shmem_get_pages_sgt(to_drm_gem_shmem_obj(fb->obj[0]));\n"
           + TAB + TAB + "if (!IS_ERR_OR_NULL(s9p_sgt))\n"
           + TAB + TAB + TAB + "dma_sync_sgtable_for_cpu(dev->dev, s9p_sgt, DMA_FROM_DEVICE);\n"
           + TAB + "}\n" + old)
    sm = sm.replace(old, new, 1)
    (src / sm_path).write_text(sm)
    print(f"{sm_path}: S9P_FB_SYNC added")
else:
    print(f"{sm_path}: S9P_FB_SYNC already there")

fb_path = "drivers/gpu/drm/sysfb/exynos-bootfb.c"
fbc = (src / fb_path).read_text()
if "S9P_FB_SYNC" not in fbc:
    inc = "#include <linux/aperture.h>\n"
    assert inc in fbc
    fbc = fbc.replace(inc, inc + "#include <linux/dma-mapping.h> /* S9P_FB_SYNC */\n", 1)
    old = TAB + "sdev = exynos_bootfb_device_create(&exynos_bootfb_driver, pdev);\n"
    assert fbc.count(old) == 1, f"{fb_path}: probe anchor matched {fbc.count(old)} times"
    new = (TAB + "ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40)); /* S9P_FB_SYNC */\n"
           + TAB + "if (ret)\n" + TAB + TAB + "return ret;\n" + old)
    fbc = fbc.replace(old, new, 1)
    (src / fb_path).write_text(fbc)
    print(f"{fb_path}: S9P_FB_SYNC dma mask added")
else:
    print(f"{fb_path}: S9P_FB_SYNC already there")

# --- HSI2C: the Exynos 9810 timing model --------------------------------------
# The 9810's controller is the "AutoV9" generation: SCL = IPCLK / ((CLK_DIV+1)
# * 16), CLK_DIV in TIMING_FS3[23:16], and the high-period / start-hold shapes
# are bit patterns in TIMING_FS2[7:0] and TIMING_FS1[23:16] (vendor
# exynos5_i2c_set_timing, fast-speed branch). The Exynos 8895 formula wrote
# CLK_DIV = 0 here, i.e. a 12.5 MHz SCL, and the master sat in WAIT for ever.
if "S9P_I2C_TIMING" not in i2c:
    old = (
        TAB + "if (i2c->variant->hw == I2C_TYPE_EXYNOSAUTOV9) {\n"
        + TAB + TAB + "div = ((clkin / (16 * i2c->op_clock)) - 1);\n"
        + TAB + TAB + "i2c_timing_s3 = div << 16;\n"
        + TAB + TAB + "if (hs_timings)\n"
        + TAB + TAB + TAB + "writel(i2c_timing_s3, i2c->regs + HSI2C_TIMING_HS3);\n"
        + TAB + TAB + "else\n"
        + TAB + TAB + TAB + "writel(i2c_timing_s3, i2c->regs + HSI2C_TIMING_FS3);\n"
        + "\n"
        + TAB + TAB + "return 0;\n"
        + TAB + "}\n"
    )
    assert i2c.count(old) == 1, f"{i2c_path}: autov9 timing block matched {i2c.count(old)} times"
    new = (
        TAB + "if (i2c->variant->hw == I2C_TYPE_EXYNOSAUTOV9) { /* S9P_I2C_TIMING */\n"
        + TAB + TAB + "unsigned int mhz = clkin / 1000000, n, s1, s2, s3;\n"
        + TAB + TAB + "void __iomem *r1 = i2c->regs + (hs_timings ? HSI2C_TIMING_HS1 : HSI2C_TIMING_FS1);\n"
        + TAB + TAB + "void __iomem *r2 = i2c->regs + (hs_timings ? HSI2C_TIMING_HS2 : HSI2C_TIMING_FS2);\n"
        + TAB + TAB + "void __iomem *r3 = i2c->regs + (hs_timings ? HSI2C_TIMING_HS3 : HSI2C_TIMING_FS3);\n"
        + "\n"
        + TAB + TAB + "/* vendor: div = ipclk / (op * 15), shapes from ipclk in MHz */\n"
        + TAB + TAB + "div = (clkin / (op_clk * 15)) & 0xff;\n"
        + TAB + TAB + "if (hs_timings)\n"
        + TAB + TAB + TAB + "n = (7 * mhz) / ((div + 1) * 100);\n"
        + TAB + TAB + "else\n"
        + TAB + TAB + TAB + "n = (9 * mhz) / ((div + 1) * 10);\n"
        + TAB + TAB + "if (n > 7)\n"
        + TAB + TAB + TAB + "n = 7;\n"
        + TAB + TAB + "s2 = ((0xffffffffu >> n) << n) & 0xff;\t\t/* TSCL_H */\n"
        + TAB + TAB + "n = n ? n - 1 : 0;\n"
        + TAB + TAB + "s1 = ((0xffffffffu >> n) << n) & 0xff;\t\t/* TSTART_HD */\n"
        + TAB + TAB + "s3 = (readl(r3) & ~0x00ff0000) | (div << 16);\n"
        + TAB + TAB + "writel(s3, r3);\n"
        + TAB + TAB + "writel((readl(r2) & ~0xff) | s2, r2);\n"
        + TAB + TAB + "writel((readl(r1) & ~0x00ff0000) | (s1 << 16), r1);\n"
        + TAB + TAB + "dev_info(i2c->dev, \"S9P timing %s: ipclk=%u op=%u div=%u -> scl=%u Hz, t1=%08x t2=%08x t3=%08x\\n\",\n"
        + TAB + TAB + TAB + " hs_timings ? \"HS\" : \"FS\", clkin, op_clk, div,\n"
        + TAB + TAB + TAB + " clkin / ((div + 1) * 16), readl(r1), readl(r2), readl(r3));\n"
        + TAB + TAB + "return 0;\n"
        + TAB + "}\n"
    )
    i2c = i2c.replace(old, new, 1)
    (src / i2c_path).write_text(i2c)
    print(f"{i2c_path}: S9P_I2C_TIMING added")
else:
    print(f"{i2c_path}: S9P_I2C_TIMING already there")

# --- ACPM mailbox (S2MPS18 over the APM firmware): voltages -----------------
insert_after(
    "drivers/soc/samsung/Makefile",
    "s9p-max77705-muic.o",
    "obj-$(CONFIG_S9P_ACPM)\t\t+= s9p-acpm.o\n",
    "s9p-acpm.o",
)

# --- G3D clock steps (stock maximum by default, thermal cap lowers) --------
insert_after(
    "drivers/soc/samsung/Makefile",
    "s9p-acpm.o",
    "obj-$(CONFIG_S9P_ACPM)\t\t+= s9p-g3d.o\n",
    "s9p-g3d.o",
)

acpm_kconfig = f"""config S9P_ACPM
{TAB}bool "ACPM MFD mailbox access (Exynos 9810 S2MPS18)"
{TAB}depends on ARCH_EXYNOS
{TAB}help
{TAB}  The S2MPS18 main PMIC is behind the APM firmware; this speaks the
{TAB}  vendor acpm_ipc/acpm_mfd protocol (channel 2) so in-kernel code can
{TAB}  set BUCK rails. Used by s9p-cpufreq and s9p-g3d.

"""
soc_kc_path = "drivers/soc/samsung/Kconfig"
soc_kc = (src / soc_kc_path).read_text()
if "config S9P_ACPM" not in soc_kc:
    (src / soc_kc_path).write_text(soc_kc + acpm_kconfig)
    print(f"{soc_kc_path}: S9P_ACPM added")
else:
    print(f"{soc_kc_path}: S9P_ACPM already there")

cpufreq_mk = (src / "drivers/cpufreq/Makefile").read_text()
if "s9p-cpufreq.o" not in cpufreq_mk:
    cpufreq_mk += "obj-$(CONFIG_ARM_S9P_CPUFREQ) += s9p-cpufreq.o\n"
    (src / "drivers/cpufreq/Makefile").write_text(cpufreq_mk)
    print("drivers/cpufreq/Makefile: s9p-cpufreq.o added")
else:
    print("drivers/cpufreq/Makefile: s9p-cpufreq.o already there")

cpufreq_kc = (src / "drivers/cpufreq/Kconfig").read_text()
if "ARM_S9P_CPUFREQ" not in cpufreq_kc:
    cpufreq_kc += f"""config ARM_S9P_CPUFREQ
{TAB}bool "cpufreq for the Samsung Exynos 9810 (Galaxy S9+)"
{TAB}depends on ARM64
{TAB}select S9P_ACPM
{TAB}help
{TAB}  Scales both clusters with the vendor PLL table entries and sets the
{TAB}  BUCK rails through the APM firmware. The ACPM DVFS plugin refuses
{TAB}  this port, so the vendor's own acme path is not available.

"""
    (src / "drivers/cpufreq/Kconfig").write_text(cpufreq_kc)
    print("drivers/cpufreq/Kconfig: ARM_S9P_CPUFREQ added")
else:
    print("drivers/cpufreq/Kconfig: ARM_S9P_CPUFREQ already there")

# --- DECON scanout takeover --------------------------------------------------
drm_mk = (src / "drivers/gpu/drm/Makefile").read_text()
if "s9p/" not in drm_mk:
    drm_mk += "obj-$(CONFIG_DRM_S9P_DECON) += s9p/\n"
    (src / "drivers/gpu/drm/Makefile").write_text(drm_mk)
    print("drivers/gpu/drm/Makefile: s9p/ added")
else:
    print("drivers/gpu/drm/Makefile: s9p/ already there")

drm_kc_path = "drivers/gpu/drm/Kconfig"
drm_kc = (src / drm_kc_path).read_text()
if 's9p/Kconfig"' not in drm_kc:
    anchor = 'source "drivers/gpu/drm/xlnx/Kconfig"\n'
    assert anchor in drm_kc, f"{drm_kc_path}: xlnx anchor not found"
    drm_kc = drm_kc.replace(anchor, anchor + 'source "drivers/gpu/drm/s9p/Kconfig"\n', 1)
    (src / drm_kc_path).write_text(drm_kc)
    print(f"{drm_kc_path}: s9p/Kconfig sourced")
else:
    print(f"{drm_kc_path}: s9p/Kconfig already sourced")

# --- brcmfmac: BCM4361 (WiFi of this phone) ----------------------------------
# Markuss Broks's upstream patch (wireless-next v3, 2026-08-20), plus the two
# extra BRCMF_PCIE_DEVICE lines maintainer Arend van Spriel asked for in
# review (2G-only 0x4420 and 5G-only 0x4421 variants). Chip rambase and
# sr_capable like the 4359, per the patch's chip.c hunk. Every insert is
# guarded on its own so a partial application never blocks a build.
hw_ids = "drivers/net/wireless/broadcom/brcm80211/include/brcm_hw_ids.h"
hw_path = src / hw_ids
hw = hw_path.read_text()
changed = False
if "BRCM_CC_4361_CHIP_ID" not in hw:
    anchor = "#define BRCM_CC_4359_CHIP_ID\t\t0x4359\n"
    assert anchor in hw, f"{hw_ids}: 4359 chip anchor not found"
    hw = hw.replace(anchor, anchor +
        "#define BRCM_CC_4361_CHIP_ID\t\t0x4361\n", 1)
    changed = True
if "BRCM_PCIE_4361_DEVICE_ID" not in hw:
    anchor = "#define BRCM_PCIE_4359_DEVICE_ID\t0x43ef\n"
    assert anchor in hw, f"{hw_ids}: 4359 device id anchor not found"
    hw = hw.replace(anchor, anchor +
        "#define BRCM_PCIE_4361_DEVICE_ID\t\t0x441f\n"
        "#define BRCM_PCIE_4361_2G_DEVICE_ID\t0x4420\n"
        "#define BRCM_PCIE_4361_5G_DEVICE_ID\t0x4421\n", 1)
    changed = True
if changed:
    hw_path.write_text(hw)
    print(f"{hw_ids}: 4361 IDs added")
else:
    print(f"{hw_ids}: 4361 already there")

pcie_c = "drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c"
pc_path = src / pcie_c
pc = pc_path.read_text()
pc_changed = False
if 'BRCMF_FW_CLM_DEF(4361' not in pc:
    anchor = 'BRCMF_FW_DEF(4359, "brcmfmac4359-pcie");'
    assert anchor in pc, f"{pcie_c}: 4359 fw anchor not found"
    pc = pc.replace(anchor, anchor +
        '\nBRCMF_FW_CLM_DEF(4361, "brcmfmac4361-pcie");', 1)
    pc_changed = True
if "BRCMF_FW_ENTRY(BRCM_CC_4361_CHIP_ID" not in pc:
    anchor = "\tBRCMF_FW_ENTRY(BRCM_CC_4359_CHIP_ID, 0x000001FF, 4359),"
    assert anchor in pc, f"{pcie_c}: 4359 fw entry anchor not found"
    pc = pc.replace(anchor, anchor +
        "\n\tBRCMF_FW_ENTRY(BRCM_CC_4361_CHIP_ID, 0xFFFFFFFF, 4361),", 1)
    pc_changed = True
if "BRCM_PCIE_4361_DEVICE_ID, WCC" not in pc:
    anchor = "\tBRCMF_PCIE_DEVICE(BRCM_PCIE_4359_DEVICE_ID, WCC),"
    assert anchor in pc, f"{pcie_c}: 4359 device anchor not found"
    pc = pc.replace(anchor, anchor +
        "\n\tBRCMF_PCIE_DEVICE(BRCM_PCIE_4361_DEVICE_ID, WCC),"
        "\n\tBRCMF_PCIE_DEVICE(BRCM_PCIE_4361_2G_DEVICE_ID, WCC),"
        "\n\tBRCMF_PCIE_DEVICE(BRCM_PCIE_4361_5G_DEVICE_ID, WCC),", 1)
    pc_changed = True
# A cold-started chip can enumerate and still answer its first BAR read with
# all ones (chip_recognition "MMIO read failed" -> -ENODEV) when the host
# gets to it too early. Deferring the probe lets the driver core try again
# later in the boot instead of giving up on the only WLAN there is.
if "brcmf_pcie_probe: defer" not in pc:
    anchor = ("\t\tret = PTR_ERR(devinfo->ci);\n"
              "\t\tdevinfo->ci = NULL;\n"
              "\t\tgoto fail;\n")
    assert anchor in pc, f"{pcie_c}: chip_attach failure anchor not found"
    pc = pc.replace(anchor,
        "\t\tret = PTR_ERR(devinfo->ci);\n"
        "\t\tif (ret == -ENODEV)\t/* brcmf_pcie_probe: defer, chip not ready yet */\n"
        "\t\t\tret = -EPROBE_DEFER;\n"
        "\t\tdevinfo->ci = NULL;\n"
        "\t\tgoto fail;\n", 1)
    pc_changed = True
if pc_changed:
    pc_path.write_text(pc)
    print(f"{pcie_c}: 4361 support added")
else:
    print(f"{pcie_c}: 4361 already there")

chip_c = "drivers/net/wireless/broadcom/brcm80211/brcmfmac/chip.c"
ch_path = src / chip_c
ch = ch_path.read_text()
ch_orig = ch
case_4359 = "case BRCM_CC_4359_CHIP_ID:"
case_4361 = "case BRCM_CC_4361_CHIP_ID:"
# tcm_rambase(): the vendor driver files the 4361 under the 4347 chip class
# (bcmdhd siutils.c) and loads it at CR4_4347_RAM_BASE = 0x170000
# (dhd_pcie.c) -- not at the 4359's rev-dependent 0x180000/0x160000 that the
# upstream patch used. 64 KB off is exactly "FW failed to initialize" with
# nothing else to show. Any earlier placement in the function is undone
# first, so the edit is idempotent.
head, sep, tail = ch.partition("static u32 brcmf_chip_tcm_rambase(")
assert sep, f"{chip_c}: tcm_rambase not found"
fn_end = tail.index("\n}\n")
fn, rest = tail[:fn_end], tail[fn_end:]
fn = re.sub(r"\tcase BRCM_CC_4361_CHIP_ID:[^\n]*\n", "", fn)
anchor = "\tcase BRCM_CC_43751_CHIP_ID:\n"
assert anchor in fn, f"{chip_c}: 43751 rambase anchor not found"
fn = fn.replace(anchor,
                "\t" + case_4361 + "\t/* 4347 class: CR4_4347_RAM_BASE */\n"
                + anchor, 1)
ch = head + sep + fn + rest
# sr_capable(): the retention_ctl check, like the 4359 (last occurrence).
idx = ch.rfind(case_4359)
assert idx >= 0
fn_start = ch.rfind("bool brcmf_chip_sr_capable(", 0, idx)
if case_4361 not in ch[fn_start:idx]:
    ch = ch[:idx] + case_4361 + "\n\t" + ch[idx:]
ch = ch.replace(case_4361 + "\n\t\t" + case_4359, case_4361 + "\n\t" + case_4359)
if ch != ch_orig:
    ch_path.write_text(ch)
    print(f"{chip_c}: 4361 rambase 0x170000 / sr_capable set")
else:
    print(f"{chip_c}: 4361 already there")

# --- brcmfmac: stamp H2D work items like Broadcom's DHD does -----------------
# Every host-to-dongle ring item carries a per-ring sequence number in the
# common header's 4th byte (DHD: cmn_msg_hdr_t.epoch = seqnum % 253, seqnum
# starting at 254) and a phase bit (flags bit 7, flipped whenever an
# allocation lands on the ring base). brcmfmac leaves both at zero. The
# BCM4361 B2 firmware verifies them: every unstamped item on the control
# ring came back as ring status BADOPTION (3) and the first ioctl timed out.
# Stamping happens centrally in brcmf_commonring_write_complete(), after the
# callers filled their headers; D2H rings are never written by the host, so
# only H2D rings are touched. Firmware that does not check ignores the bytes.
cr_h = "drivers/net/wireless/broadcom/brcm80211/brcmfmac/commonring.h"
cr_h_path = src / cr_h
crh = cr_h_path.read_text()
if "stamp_ptr" not in crh:
    anchor = "\tbool was_full;\n"
    assert anchor in crh, f"{cr_h}: was_full anchor not found"
    crh = crh.replace(anchor, anchor +
        "\n\t/* H2D item stamping: next unstamped item, phase bit, sequence */\n"
        "\tu16 stamp_ptr;\n"
        "\tu8 phase;\n"
        "\tu32 seqnum;\n", 1)
    cr_h_path.write_text(crh)
    print(f"{cr_h}: stamp fields added")
else:
    print(f"{cr_h}: stamp fields already there")

cr_c = "drivers/net/wireless/broadcom/brcm80211/brcmfmac/commonring.c"
cr_c_path = src / cr_c
crc = cr_c_path.read_text()
if "brcmf_commonring_stamp" not in crc:
    anchor = "void brcmf_commonring_config(struct brcmf_commonring *commonring, u16 depth,"
    assert anchor in crc, f"{cr_c}: config anchor not found"
    helper = (
        "/*\n"
        " * Every host-to-dongle work item carries a per-ring sequence number in\n"
        " * the common header's 4th byte (\"epoch\") and a phase bit in the flags\n"
        " * byte, the way Broadcom's DHD stamps them: epoch = seqnum % 253 with\n"
        " * seqnum starting at 254, phase (bit 7) flipped whenever an allocation\n"
        " * lands on the ring base. Firmware that verifies them (BCM4361 B2)\n"
        " * rejects unstamped items with ring status BADOPTION.\n"
        " */\n"
        "#define BRCMF_H2D_EPOCH_MODULO\t253\n"
        "#define BRCMF_H2D_EPOCH_INIT\t(BRCMF_H2D_EPOCH_MODULO + 1)\n"
        "#define BRCMF_H2D_PHASE_BIT\t0x80\n"
        "\n"
        "static void brcmf_commonring_stamp(struct brcmf_commonring *commonring)\n"
        "{\n"
        "\tu8 *item;\n"
        "\n"
        "\twhile (commonring->stamp_ptr != commonring->w_ptr) {\n"
        "\t\tif (commonring->stamp_ptr == 0)\n"
        "\t\t\tcommonring->phase ^= BRCMF_H2D_PHASE_BIT;\n"
        "\t\titem = commonring->buf_addr +\n"
        "\t\t       commonring->stamp_ptr * commonring->item_len;\n"
        "\t\titem[2] = (item[2] & ~BRCMF_H2D_PHASE_BIT) | commonring->phase;\n"
        "\t\titem[3] = commonring->seqnum % BRCMF_H2D_EPOCH_MODULO;\n"
        "\t\tcommonring->seqnum++;\n"
        "\t\tcommonring->stamp_ptr++;\n"
        "\t\tif (commonring->stamp_ptr == commonring->depth)\n"
        "\t\t\tcommonring->stamp_ptr = 0;\n"
        "\t}\n"
        "}\n"
        "\n"
    )
    crc = crc.replace(anchor, helper + anchor, 1)
    anchor = "\tcommonring->f_ptr = 0;\n}\n"
    assert anchor in crc, f"{cr_c}: config f_ptr anchor not found"
    crc = crc.replace(anchor,
        "\tcommonring->f_ptr = 0;\n"
        "\tcommonring->stamp_ptr = 0;\n"
        "\tcommonring->phase = 0;\n"
        "\tcommonring->seqnum = BRCMF_H2D_EPOCH_INIT;\n"
        "}\n", 1)
    anchor = "int brcmf_commonring_write_complete(struct brcmf_commonring *commonring)\n{\n"
    assert anchor in crc, f"{cr_c}: write_complete anchor not found"
    crc = crc.replace(anchor, anchor + "\tbrcmf_commonring_stamp(commonring);\n\n", 1)
    cr_c_path.write_text(crc)
    print(f"{cr_c}: H2D item stamping added")
else:
    print(f"{cr_c}: H2D item stamping already there")

# --- PCIe root complex for the Exynos 9810 (WiFi channel, BCM4361) -----------
insert_after(
    "drivers/pci/controller/dwc/Makefile",
    "obj-$(CONFIG_PCI_EXYNOS) += pci-exynos.o\n",
    "obj-$(CONFIG_PCIE_EXYNOS9810) += pcie-exynos9810.o\n",
    "PCIE_EXYNOS9810",
)

pcie_kconfig = f"""config PCIE_EXYNOS9810
{TAB}tristate "Samsung Exynos 9810 PCIe host controller (WiFi)"
{TAB}depends on PCI && OF && ARM64 && PCI_MSI
{TAB}select PCIE_DW_HOST
{TAB}select PCIE_DW
{TAB}help
{TAB}  PCIe channel 0 of the Exynos 9810, wired to the WiFi module on the
{TAB}  Galaxy S9/S9+. The PHY is programmed with Samsung's own CAL sequence
{TAB}  from the driver; the single endpoint is served with MSI only.

"""
insert_before(
    "drivers/pci/controller/dwc/Kconfig",
    "config PCIE_DW_EP\n",
    pcie_kconfig,
    "PCIE_EXYNOS9810",
)

# --- M3 cluster: vendor PMU defaults before PSCI CPU_ON ----------------------
# Without these six writes CPU4-7 take CPU_ON (returns 0) but never reach
# secondary_start_kernel: "CPU4: failed to come online", "failed in unknown
# state : 0x0". They are the CPUCL1 entries of pmucal_lpm_init in Samsung's
# 4.9.59 kernel (cal_if_init -> pmucal_system_init, before SMP). Cold-boot
# values from sboot: 0x1088 = c7150000, the five OPTION registers = 1, also
# after a warm reboot. Enabled by s9p_vendor_m3_defaults; the M3 must come up
# in the initial SMP bring-up (maxcpus=8), hotplug still fails.
insert_after(
    "arch/arm64/kernel/psci.c",
    "#include <linux/of.h>\n",
    "#include <linux/io.h>\n",
    "#include <linux/io.h>",
)

m3_defaults = """/* S9P-VENDOR-DEFAULTS: Samsung 4.9.59 pmucal_lpm_init, CPUCL1 entries. */
static bool s9p_vendor_m3_defaults;

static int __init s9p_vendor_m3_defaults_param(char *unused)
{
	s9p_vendor_m3_defaults = true;
	return 0;
}
early_param("s9p_vendor_m3_defaults", s9p_vendor_m3_defaults_param);

static int s9p_apply_vendor_m3_defaults(unsigned int cpu)
{
	static const struct {
		u32 offset, mask, value;
	} seq[] = {
		/* CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG */
		{ 0x1088, 0xff000000, 0x8d000000 },
		/* MEMORY_CLUSTER1_CPU[0..3]_OPTION and NONCPU_OPTION */
		{ 0x2114, 3, 2 },
		{ 0x2180, 3, 2 },
		{ 0x21ec, 3, 2 },
		{ 0x2258, 3, 2 },
		{ 0x2384, 3, 2 },
	};
	void __iomem *pmu;
	unsigned int i;
	int err = 0;

	if (!s9p_vendor_m3_defaults ||
	    !of_machine_is_compatible("samsung,exynos9810") ||
	    (cpu_logical_map(cpu) & ~3ULL) != 0x100)
		return 0;
	pmu = ioremap(0x14060000, 0x8000);
	if (!pmu)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		u32 before = readl(pmu + seq[i].offset);
		u32 after;

		writel((before & ~seq[i].mask) | seq[i].value,
		       pmu + seq[i].offset);
		after = readl(pmu + seq[i].offset);
		pr_info("S9P vendor PMU %04x: %08x -> %08x mask=%08x\\n",
			seq[i].offset, before, after, seq[i].mask);
		if ((after & seq[i].mask) != seq[i].value) {
			err = -EIO;
			break;
		}
	}
	dsb(sy);
	iounmap(pmu);
	return err;
}
/* S9P-VENDOR-DEFAULTS-END */

"""
insert_before(
    "arch/arm64/kernel/psci.c",
    "static int cpu_psci_cpu_boot(unsigned int cpu)\n",
    m3_defaults,
    "S9P-VENDOR-DEFAULTS",
)

psci = src / "arch/arm64/kernel/psci.c"
s = psci.read_text()
old_on = "\tint err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);\n"
new_on = ("\tint err = s9p_apply_vendor_m3_defaults(cpu);\n\n"
          "\tif (err)\n\t\treturn err;\n"
          "\terr = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);\n")
if new_on in s:
    print("arch/arm64/kernel/psci.c: M3 defaults call already there")
else:
    assert old_on in s, "arch/arm64/kernel/psci.c: cpu_on line not found"
    psci.write_text(s.replace(old_on, new_on, 1))
    print("arch/arm64/kernel/psci.c: M3 defaults call added")

# --- CPU power management hooks for extras/cpu-idle -------------------------
# Samsung's EL3 and APM firmware power a core down only when the kernel has
# told the PMU first (CPU_INFORM, the core's GRP2 wake-up interrupt), which
# the vendor kernel does in exynos-powermode.c around every PSCI CPU_SUSPEND
# and CPU_OFF. Mainline has no place for that, so: three hooks, NULL unless
# a module (src/cpupm/s9p-cpupm.c) fills them, and a PSCI suspend entry that
# always saves the CPU context. The Exynos power states for cluster and
# system sleep (0x01000000, 0x03000000, vendor psci_suspend_customized_
# finisher) have the PSCI type bit clear, which mainline reads as "context
# kept"; they do lose it.
psci_fw = src / "drivers/firmware/psci/psci.c"
s = psci_fw.read_text()
if "S9P-SUSPEND-LOSSY" not in s:
    s += """
/* S9P-SUSPEND-LOSSY: PSCI CPU_SUSPEND with the context saved, any state. */
int s9p_psci_suspend_lossy(u32 state)
{
	return cpu_suspend(state, psci_suspend_finisher);
}
EXPORT_SYMBOL_GPL(s9p_psci_suspend_lossy);
"""
    psci_fw.write_text(s)
    print("drivers/firmware/psci/psci.c: S9P-SUSPEND-LOSSY added")

hooks_h = """
/* S9P-CPU-HOOKS: filled by extras/cpu-idle (s9p-cpupm), NULL otherwise. */
struct s9p_cpu_hooks {
	void (*die)(unsigned int cpu);		/* on the dying CPU, before CPU_OFF */
	int (*kill)(unsigned int cpu);		/* on a live CPU, after it died */
	void (*starting)(unsigned int cpu);	/* on a new CPU, before CPU_STARTING */
};
extern struct s9p_cpu_hooks s9p_cpu_hooks;
int s9p_psci_suspend_lossy(u32 state);
"""
insert_before(
    "include/linux/psci.h",
    "#if defined(CONFIG_ARM_PSCI_FW)\n",
    hooks_h,
    "S9P-CPU-HOOKS",
)

hooks = """/* S9P-CPU-HOOKS */
struct s9p_cpu_hooks s9p_cpu_hooks;
EXPORT_SYMBOL_GPL(s9p_cpu_hooks);
/* S9P-CPU-HOOKS-END */

"""
insert_before(
    "arch/arm64/kernel/psci.c",
    "static int cpu_psci_cpu_boot(unsigned int cpu)\n",
    hooks,
    "S9P-CPU-HOOKS",
)
psci = src / "arch/arm64/kernel/psci.c"
s = psci.read_text()
die_old = "\tpsci_ops.cpu_off(state);\n}\n"
die_new = "\tif (s9p_cpu_hooks.die)\n\t\ts9p_cpu_hooks.die(cpu);\n\tpsci_ops.cpu_off(state);\n}\n"
if die_new not in s:
    assert die_old in s, "arch/arm64/kernel/psci.c: cpu_off call not found"
    s = s.replace(die_old, die_new, 1)
kill_old = "\tif (!psci_ops.affinity_info)\n\t\treturn 0;\n"
kill_new = "\tif (s9p_cpu_hooks.kill)\n\t\treturn s9p_cpu_hooks.kill(cpu);\n" + kill_old
if kill_new not in s:
    assert kill_old in s, "arch/arm64/kernel/psci.c: affinity_info check not found"
    s = s.replace(kill_old, kill_new, 1)
psci.write_text(s)
print("arch/arm64/kernel/psci.c: S9P die/kill hooks in place")

smp = src / "arch/arm64/kernel/smp.c"
s = smp.read_text()
start_old = "\tnotify_cpu_starting(cpu);\n"
start_new = ("\tif (s9p_cpu_hooks.starting)\n\t\ts9p_cpu_hooks.starting(cpu);\n"
             + start_old)
if "s9p_cpu_hooks.starting" not in s:
    assert start_old in s, "arch/arm64/kernel/smp.c: notify_cpu_starting not found"
    smp.write_text(s.replace(start_old, start_new, 1))
    print("arch/arm64/kernel/smp.c: S9P starting hook added")

# --- MCT on arm64: broadcast device only, keep the arch timer's sched_clock --
# The MCT runs at the arch timer's 26 MHz; sched_clock_register() takes an
# equal rate as a replacement, which would move sched_clock from the 56-bit
# CNTVCT read to a 32-bit MMIO read. The clocksource and clockevents ratings
# already keep the arch timer in charge on arm64.
mct = src / "drivers/clocksource/exynos_mct.c"
s = mct.read_text()
old = "\tsched_clock_register(exynos4_read_sched_clock, 32, clk_rate);\n"
new = ("\t/* S9P: on arm64 the arch timer stays sched_clock */\n"
       "\tif (!IS_ENABLED(CONFIG_ARM64))\n"
       "\t\tsched_clock_register(exynos4_read_sched_clock, 32, clk_rate);\n")
if new not in s:
    assert old in s, "exynos_mct.c: sched_clock_register not found"
    mct.write_text(s.replace(old, new, 1))
    print("drivers/clocksource/exynos_mct.c: S9P sched_clock kept on the arch timer")

# --- MCT local timers as the per-CPU tick on arm64 --------------------------
# The vendor kernel ticks every core from its MCT local timer (an SPI routed
# to that core, in the always-on domain) and keeps the arch timer as
# clocksource only; a core powered down in C2 then still gets its own timer
# interrupt. The broadcast scheme (MCT global comparator + IPIs) lost wake-ups:
# a core in C2 stayed off for good after a few hundred entries. So on arm64
# the local timers rank above the arch timer (450), the clocksource stays
# below it (350, the arch counter is far cheaper to read).
s = mct.read_text()
old = "#define MCT_CLKSOURCE_RATING\t\t350\n#define MCT_CLKEVENTS_RATING\t\t350\n"
new = "#define MCT_CLKSOURCE_RATING\t\t350\n/* S9P: per-CPU tick on the MCT, as the vendor kernel */\n#define MCT_CLKEVENTS_RATING\t\t500\n"
if new not in s:
    assert old in s, "exynos_mct.c: arm64 ratings not found"
    mct.write_text(s.replace(old, new, 1))
    print("drivers/clocksource/exynos_mct.c: S9P local timers as the tick")

print("PATCH_TREE_OK")

# --- watchdog: keep watching through shutdown and reboot ---------------------
# Mainline's s3c2410wdt_shutdown() masks the reset request and stops the
# timer, and the watchdog core stops it on the reboot notifier. Anything that
# then wedges -- a driver's shutdown hook, the reset itself -- leaves the
# phone dead with nothing to restart it: measured 2026-10-17, a reboot after a
# module oops sat dark for 15 minutes until Power + Volume Down. Here the
# timer is fed once more (a full timeout of margin) and left running; the
# command line sets watchdog.stop_on_reboot=0 for the core and
# s3c2410_wdt.nowayout=1 against systemd-shutdown's magic close. A reset that
# does happen lands in sboot as before, which re-arms the watchdog itself.
wdt = src / "drivers/watchdog/s3c2410_wdt.c"
s = wdt.read_text()
old = """static void s3c2410wdt_shutdown(struct platform_device *dev)
{
	struct s3c2410_wdt *wdt = platform_get_drvdata(dev);

	s3c2410wdt_enable(wdt, false);
	s3c2410wdt_stop(&wdt->wdt_device);
}"""
new = """static void s3c2410wdt_shutdown(struct platform_device *dev)
{
	struct s3c2410_wdt *wdt = platform_get_drvdata(dev);

	/* S9P-WDT-SHUTDOWN: keep the reset armed through shutdown and reboot */
	if (watchdog_active(&wdt->wdt_device) ||
	    watchdog_hw_running(&wdt->wdt_device)) {
		s3c2410wdt_keepalive(&wdt->wdt_device);
		return;
	}
	s3c2410wdt_enable(wdt, false);
	s3c2410wdt_stop(&wdt->wdt_device);
}"""
if "S9P-WDT-SHUTDOWN" in s:
    print("s3c2410_wdt.c: S9P-WDT-SHUTDOWN already there")
else:
    assert old in s, "s3c2410_wdt.c: shutdown hook not found"
    wdt.write_text(s.replace(old, new, 1))
    print("s3c2410_wdt.c: S9P-WDT-SHUTDOWN added")
