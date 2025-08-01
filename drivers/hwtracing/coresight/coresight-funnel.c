// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011-2012, The Linux Foundation. All rights reserved.
 *
 * Description: CoreSight Funnel driver
 */

#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/coresight.h>
#include <linux/amba/bus.h>
#include <linux/clk.h>

#include "coresight-priv.h"

#define FUNNEL_FUNCTL		0x000
#define FUNNEL_PRICTL		0x004

#define FUNNEL_HOLDTIME_MASK	0xf00
#define FUNNEL_HOLDTIME_SHFT	0x8
#define FUNNEL_HOLDTIME		(0x7 << FUNNEL_HOLDTIME_SHFT)
#define FUNNEL_ENSx_MASK	0xff

DEFINE_CORESIGHT_DEVLIST(funnel_devs, "funnel");

/**
 * struct funnel_drvdata - specifics associated to a funnel component
 * @base:	memory mapped base address for this component.
 * @atclk:	optional clock for the core parts of the funnel.
 * @pclk:	APB clock if present, otherwise NULL
 * @csdev:	component vitals needed by the framework.
 * @priority:	port selection order.
 * @spinlock:	serialize enable/disable operations.
 */
struct funnel_drvdata {
	void __iomem		*base;
	struct clk		*atclk;
	struct clk		*pclk;
	struct coresight_device	*csdev;
	unsigned long		priority;
	raw_spinlock_t		spinlock;
};

static int dynamic_funnel_enable_hw(struct funnel_drvdata *drvdata, int port)
{
	u32 functl;
	int rc = 0;
	struct coresight_device *csdev = drvdata->csdev;

	CS_UNLOCK(drvdata->base);

	functl = readl_relaxed(drvdata->base + FUNNEL_FUNCTL);
	/* Claim the device only when we enable the first slave */
	if (!(functl & FUNNEL_ENSx_MASK)) {
		rc = coresight_claim_device_unlocked(csdev);
		if (rc)
			goto done;
	}

	functl &= ~FUNNEL_HOLDTIME_MASK;
	functl |= FUNNEL_HOLDTIME;
	functl |= (1 << port);
	writel_relaxed(functl, drvdata->base + FUNNEL_FUNCTL);
	writel_relaxed(drvdata->priority, drvdata->base + FUNNEL_PRICTL);
done:
	CS_LOCK(drvdata->base);
	return rc;
}

static int funnel_enable(struct coresight_device *csdev,
			 struct coresight_connection *in,
			 struct coresight_connection *out)
{
	int rc = 0;
	struct funnel_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	unsigned long flags;
	bool first_enable = false;

	raw_spin_lock_irqsave(&drvdata->spinlock, flags);
	if (in->dest_refcnt == 0) {
		if (drvdata->base)
			rc = dynamic_funnel_enable_hw(drvdata, in->dest_port);
		if (!rc)
			first_enable = true;
	}
	if (!rc)
		in->dest_refcnt++;
	raw_spin_unlock_irqrestore(&drvdata->spinlock, flags);

	if (first_enable)
		dev_dbg(&csdev->dev, "FUNNEL inport %d enabled\n",
			in->dest_port);
	return rc;
}

static void dynamic_funnel_disable_hw(struct funnel_drvdata *drvdata,
				      int inport)
{
	u32 functl;
	struct coresight_device *csdev = drvdata->csdev;

	CS_UNLOCK(drvdata->base);

	functl = readl_relaxed(drvdata->base + FUNNEL_FUNCTL);
	functl &= ~(1 << inport);
	writel_relaxed(functl, drvdata->base + FUNNEL_FUNCTL);

	/* Disclaim the device if none of the slaves are now active */
	if (!(functl & FUNNEL_ENSx_MASK))
		coresight_disclaim_device_unlocked(csdev);

	CS_LOCK(drvdata->base);
}

static void funnel_disable(struct coresight_device *csdev,
			   struct coresight_connection *in,
			   struct coresight_connection *out)
{
	struct funnel_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	unsigned long flags;
	bool last_disable = false;

	raw_spin_lock_irqsave(&drvdata->spinlock, flags);
	if (--in->dest_refcnt == 0) {
		if (drvdata->base)
			dynamic_funnel_disable_hw(drvdata, in->dest_port);
		last_disable = true;
	}
	raw_spin_unlock_irqrestore(&drvdata->spinlock, flags);

	if (last_disable)
		dev_dbg(&csdev->dev, "FUNNEL inport %d disabled\n",
			in->dest_port);
}

static const struct coresight_ops_link funnel_link_ops = {
	.enable		= funnel_enable,
	.disable	= funnel_disable,
};

static const struct coresight_ops funnel_cs_ops = {
	.link_ops	= &funnel_link_ops,
};

static ssize_t priority_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct funnel_drvdata *drvdata = dev_get_drvdata(dev->parent);
	unsigned long val = drvdata->priority;

	return sprintf(buf, "%#lx\n", val);
}

static ssize_t priority_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t size)
{
	int ret;
	unsigned long val;
	struct funnel_drvdata *drvdata = dev_get_drvdata(dev->parent);

	ret = kstrtoul(buf, 16, &val);
	if (ret)
		return ret;

	drvdata->priority = val;
	return size;
}
static DEVICE_ATTR_RW(priority);

static u32 get_funnel_ctrl_hw(struct funnel_drvdata *drvdata)
{
	u32 functl;

	CS_UNLOCK(drvdata->base);
	functl = readl_relaxed(drvdata->base + FUNNEL_FUNCTL);
	CS_LOCK(drvdata->base);

	return functl;
}

static ssize_t funnel_ctrl_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	u32 val;
	struct funnel_drvdata *drvdata = dev_get_drvdata(dev->parent);

	pm_runtime_get_sync(dev->parent);

	val = get_funnel_ctrl_hw(drvdata);

	pm_runtime_put(dev->parent);

	return sprintf(buf, "%#x\n", val);
}
static DEVICE_ATTR_RO(funnel_ctrl);

static struct attribute *coresight_funnel_attrs[] = {
	&dev_attr_funnel_ctrl.attr,
	&dev_attr_priority.attr,
	NULL,
};
ATTRIBUTE_GROUPS(coresight_funnel);

static int funnel_probe(struct device *dev, struct resource *res)
{
	int ret;
	void __iomem *base;
	struct coresight_platform_data *pdata = NULL;
	struct funnel_drvdata *drvdata;
	struct coresight_desc desc = { 0 };

	if (is_of_node(dev_fwnode(dev)) &&
	    of_device_is_compatible(dev->of_node, "arm,coresight-funnel"))
		dev_warn_once(dev, "Uses OBSOLETE CoreSight funnel binding\n");

	desc.name = coresight_alloc_device_name(&funnel_devs, dev);
	if (!desc.name)
		return -ENOMEM;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->atclk = devm_clk_get(dev, "atclk"); /* optional */
	if (!IS_ERR(drvdata->atclk)) {
		ret = clk_prepare_enable(drvdata->atclk);
		if (ret)
			return ret;
	}

	drvdata->pclk = coresight_get_enable_apb_pclk(dev);
	if (IS_ERR(drvdata->pclk))
		return -ENODEV;

	/*
	 * Map the device base for dynamic-funnel, which has been
	 * validated by AMBA core.
	 */
	if (res) {
		base = devm_ioremap_resource(dev, res);
		if (IS_ERR(base)) {
			ret = PTR_ERR(base);
			goto out_disable_clk;
		}
		drvdata->base = base;
		desc.groups = coresight_funnel_groups;
		desc.access = CSDEV_ACCESS_IOMEM(base);
	}

	dev_set_drvdata(dev, drvdata);

	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata)) {
		ret = PTR_ERR(pdata);
		goto out_disable_clk;
	}
	dev->platform_data = pdata;

	raw_spin_lock_init(&drvdata->spinlock);
	desc.type = CORESIGHT_DEV_TYPE_LINK;
	desc.subtype.link_subtype = CORESIGHT_DEV_SUBTYPE_LINK_MERG;
	desc.ops = &funnel_cs_ops;
	desc.pdata = pdata;
	desc.dev = dev;
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev)) {
		ret = PTR_ERR(drvdata->csdev);
		goto out_disable_clk;
	}

	ret = 0;

out_disable_clk:
	if (ret && !IS_ERR_OR_NULL(drvdata->atclk))
		clk_disable_unprepare(drvdata->atclk);
	if (ret && !IS_ERR_OR_NULL(drvdata->pclk))
		clk_disable_unprepare(drvdata->pclk);
	return ret;
}

static int funnel_remove(struct device *dev)
{
	struct funnel_drvdata *drvdata = dev_get_drvdata(dev);

	coresight_unregister(drvdata->csdev);

	return 0;
}

#ifdef CONFIG_PM
static int funnel_runtime_suspend(struct device *dev)
{
	struct funnel_drvdata *drvdata = dev_get_drvdata(dev);

	if (drvdata && !IS_ERR(drvdata->atclk))
		clk_disable_unprepare(drvdata->atclk);

	if (drvdata && !IS_ERR_OR_NULL(drvdata->pclk))
		clk_disable_unprepare(drvdata->pclk);

	return 0;
}

static int funnel_runtime_resume(struct device *dev)
{
	struct funnel_drvdata *drvdata = dev_get_drvdata(dev);

	if (drvdata && !IS_ERR(drvdata->atclk))
		clk_prepare_enable(drvdata->atclk);

	if (drvdata && !IS_ERR_OR_NULL(drvdata->pclk))
		clk_prepare_enable(drvdata->pclk);
	return 0;
}
#endif

static const struct dev_pm_ops funnel_dev_pm_ops = {
	SET_RUNTIME_PM_OPS(funnel_runtime_suspend, funnel_runtime_resume, NULL)
};

static int funnel_platform_probe(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	int ret;

	pm_runtime_get_noresume(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = funnel_probe(&pdev->dev, res);
	pm_runtime_put(&pdev->dev);
	if (ret)
		pm_runtime_disable(&pdev->dev);

	return ret;
}

static void funnel_platform_remove(struct platform_device *pdev)
{
	struct funnel_drvdata *drvdata = dev_get_drvdata(&pdev->dev);

	if (WARN_ON(!drvdata))
		return;

	funnel_remove(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	if (!IS_ERR_OR_NULL(drvdata->pclk))
		clk_put(drvdata->pclk);
}

static const struct of_device_id funnel_match[] = {
	{.compatible = "arm,coresight-static-funnel"},
	{}
};

MODULE_DEVICE_TABLE(of, funnel_match);

#ifdef CONFIG_ACPI
static const struct acpi_device_id funnel_acpi_ids[] = {
	{"ARMHC9FE", 0, 0, 0}, /* ARM Coresight Static Funnel */
	{"ARMHC9FF", 0, 0, 0}, /* ARM CoreSight Dynamic Funnel */
	{},
};

MODULE_DEVICE_TABLE(acpi, funnel_acpi_ids);
#endif

static struct platform_driver funnel_driver = {
	.probe		= funnel_platform_probe,
	.remove		= funnel_platform_remove,
	.driver		= {
		.name   = "coresight-funnel",
		/* THIS_MODULE is taken care of by platform_driver_register() */
		.of_match_table = funnel_match,
		.acpi_match_table = ACPI_PTR(funnel_acpi_ids),
		.pm	= &funnel_dev_pm_ops,
		.suppress_bind_attrs = true,
	},
};

static int dynamic_funnel_probe(struct amba_device *adev,
				const struct amba_id *id)
{
	int ret;

	ret = funnel_probe(&adev->dev, &adev->res);
	if (!ret)
		pm_runtime_put(&adev->dev);

	return ret;
}

static void dynamic_funnel_remove(struct amba_device *adev)
{
	funnel_remove(&adev->dev);
}

static const struct amba_id dynamic_funnel_ids[] = {
	{
		.id     = 0x000bb908,
		.mask   = 0x000fffff,
	},
	{
		/* Coresight SoC-600 */
		.id     = 0x000bb9eb,
		.mask   = 0x000fffff,
	},
	{ 0, 0, NULL },
};

MODULE_DEVICE_TABLE(amba, dynamic_funnel_ids);

static struct amba_driver dynamic_funnel_driver = {
	.drv = {
		.name	= "coresight-dynamic-funnel",
		.pm	= &funnel_dev_pm_ops,
		.suppress_bind_attrs = true,
	},
	.probe		= dynamic_funnel_probe,
	.remove		= dynamic_funnel_remove,
	.id_table	= dynamic_funnel_ids,
};

static int __init funnel_init(void)
{
	return coresight_init_driver("funnel", &dynamic_funnel_driver, &funnel_driver,
				     THIS_MODULE);
}

static void __exit funnel_exit(void)
{
	coresight_remove_driver(&dynamic_funnel_driver, &funnel_driver);
}

module_init(funnel_init);
module_exit(funnel_exit);

MODULE_AUTHOR("Pratik Patel <pratikp@codeaurora.org>");
MODULE_AUTHOR("Mathieu Poirier <mathieu.poirier@linaro.org>");
MODULE_DESCRIPTION("Arm CoreSight Funnel Driver");
MODULE_LICENSE("GPL v2");
