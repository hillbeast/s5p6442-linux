// SPDX-License-Identifier: GPL-2.0
//
// Samsung Galaxy 3 i5800 (Apollo) Hardware Revision ID
//
// Mark Kennard <markkennard4@gmail.com>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>

unsigned int apollo_hardware_revision = 0;
EXPORT_SYMBOL_GPL(apollo_hardware_revision);

static int apollo_hw_rev_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct gpio_descs *rev_gpios;
    unsigned int rev_value = 0;
    int i;

    dev_info(dev, "Initializing Apollo hardware revision driver\n");

    rev_gpios = gpiod_get_array(dev, "rev", GPIOD_IN);
    if (IS_ERR(rev_gpios)) {
        return dev_err_probe(dev, PTR_ERR(rev_gpios), "Failed to get rev-gpios\n");
    }

    for (i = 0; i < rev_gpios->ndescs; i++) {
        int val = gpiod_get_value_cansleep(rev_gpios->desc[i]);
        if (val < 0) {
            dev_err(dev, "Failed to read GPIO index %d\n", i);
            gpiod_put_array(rev_gpios);
            return val;
        }
        
        if (val)
            rev_value |= (1 << i);
    }

    apollo_hardware_revision = rev_value;
    dev_info(dev, "Detected Hardware Revision: 0x%X (Raw pins: %d)\n", 
             apollo_hardware_revision, rev_gpios->ndescs);

    gpiod_put_array(rev_gpios);

    return 0;
}

unsigned int apollo_bootmode = 0;
EXPORT_SYMBOL_GPL(apollo_bootmode);

static int apollo_hw_bootmode_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct gpio_descs *bootmode_gpios;
    unsigned int bootmode_value = 0;
    int i;

    dev_info(dev, "Initializing Apollo hardware revision driver\n");

    bootmode_gpios = gpiod_get_array(dev, "bootmode", GPIOD_IN);
    if (IS_ERR(bootmode_gpios)) {
        return dev_err_probe(dev, PTR_ERR(bootmode_gpios), "Failed to get rev-gpios\n");
    }

    int val = gpiod_get_value_cansleep(bootmode_gpios->desc[i]);
    if (val < 0) {
        dev_err(dev, "Failed to read GPIO index %d\n", i);
        gpiod_put_array(bootmode_gpios);
        return val;
    }
    
    if (val)
        bootmode_value |= (1 << i);

    apollo_bootmode = bootmode_value;
    dev_info(dev, "Detected Boot Mode: 0x%X (Raw pins: %d)\n", 
             apollo_bootmode, bootmode_gpios->ndescs);

    gpiod_put_array(bootmode_gpios);

    return 0;
}

static int apollo_hw_probe(struct platform_device *pdev)
{
    int ret;

    ret = apollo_hw_rev_probe(pdev);
    if (ret)
        pr_err("%s: apollo_hw_rev_probe failed with response 0x%02x", __func__, ret);

    ret = apollo_hw_bootmode_probe(pdev);
    if (ret)
        pr_err("%s: apollo_bootmode_probe failed with response 0x%02x", __func__, ret);
    
    return ret;
}

static const struct of_device_id apollo_hw_rev_match[] = {
    { .compatible = "samsung,apollo_hw_rev" },
    { }
};
MODULE_DEVICE_TABLE(of, apollo_hw_rev_match);

static struct platform_driver apollo_hw_rev_driver = {
    .probe = apollo_hw_probe,
    .driver = {
        .name = "apollo_hw_rev",
        .of_match_table = apollo_hw_rev_match,
        .suppress_bind_attrs = true, 
    },
};

static int __init apollo_hw_rev_init(void)
{
    return platform_driver_register(&apollo_hw_rev_driver);
}

subsys_initcall(apollo_hw_rev_init); 

static void __exit apollo_hw_rev_exit(void)
{
    platform_driver_unregister(&apollo_hw_rev_driver);
}
module_exit(apollo_hw_rev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark Kennard");
MODULE_DESCRIPTION("Samsung Galaxy 3 Apollo HW Rev Driver");