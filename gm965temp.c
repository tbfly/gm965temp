/*
 * A hwmon driver for the Intel GM/GME965 chipset IGP
 * temperature sensors
 *
 * Copyright (C) 2009 Lu Zhihe <tombowfly at gmail.com>
 *
 * Tested and helped improved by: 
 *
 *    Tobias Hain <tobias.hain@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#define DRVNAME "gm965temp"

#define USE_RTR   /*Flag for report method*/
#define DEBUG

enum { SHOW_TEMP, SHOW_TJMAX, SHOW_TTARGET, SHOW_LABEL } SHOW;

/*Mobile Series Chipsets, read from TR1/RTR1*/
#define PCI_DEVICE_ID_INTEL_82965GM       0x2a00
#define PCI_DEVICE_ID_INTEL_82965GME      0x2a10
#define PCI_DEVICE_ID_INTEL_GM45          0x2a40

/*3 Series Chipsets, read from TSTTP.RELT*/
#define PCI_DEVICE_ID_INTEL_Q35		  0x29b0
#define PCI_DEVICE_ID_INTEL_G33		  0x29c0
#define PCI_DEVICE_ID_INTEL_Q33		  0x29d0

/*4 Series Chipsets, read from TSTTP.RELT*/
#define PCI_DEVICE_ID_INTEL_Q45		  0x2e10
#define PCI_DEVICE_ID_INTEL_G45		  0x2e20
#define PCI_DEVICE_ID_INTEL_G41		  0x2e30
#define PCI_DEVICE_ID_INTEL_B43_BASE	  0x2e40
#define PCI_DEVICE_ID_INTEL_B43_SOFT_SKU  0x2e90

#define IS_GM   (  data->chipset_id == PCI_DEVICE_ID_INTEL_82965GM	\
		|| data->chipset_id == PCI_DEVICE_ID_INTEL_82965GME 	\
		|| data->chipset_id == PCI_DEVICE_ID_INTEL_GM45 )

#define GM965_SYSFS_NAME_LEN              16

#define MCHBAR_I965 0x48      /* MCH Memory Mapped Register BAR */
#define MCHBAR_MASK 0xfffffc000ULL  /* bits 35:14 */

static ssize_t show_label(struct device *dev,
		struct device_attribute *devattr,
		char *buf);

static ssize_t show_temp(struct device *dev,
				struct device_attribute *devattr,
				char *buf);
struct gm965temp_data {
	struct device *hwmon_dev;
	struct mutex update_lock;

	u64 igp_base;
	unsigned long igp_len;
	void __iomem *igp_mmio;

	unsigned int temp;
	unsigned long chipset_id;
};

static ssize_t show_name(struct device *dev, struct device_attribute *devattr,
			 char *buf)
{
	return sprintf(buf, "%s\n", DRVNAME);
}

static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, show_temp, NULL, SHOW_TEMP);
static SENSOR_DEVICE_ATTR(temp1_crit, S_IRUGO, show_temp, NULL, SHOW_TJMAX);
static SENSOR_DEVICE_ATTR(temp1_max, S_IRUGO, show_temp, NULL,
			SHOW_TTARGET);
static SENSOR_DEVICE_ATTR(temp1_label, S_IRUGO, show_label, NULL, SHOW_LABEL);
static DEVICE_ATTR(name, S_IRUGO, show_name, NULL);

static struct attribute *gm965temp_attributes[] = {
	&sensor_dev_attr_temp1_label.dev_attr.attr,
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_temp1_crit.dev_attr.attr,
	&sensor_dev_attr_temp1_max.dev_attr.attr,
	NULL
};

static const struct attribute_group gm965temp_group = {
	.attrs = gm965temp_attributes,
};

static struct platform_device *igp_pdev;

static u8 igp_read_byte(struct gm965temp_data *data, unsigned long offset)
{
	return ioread8(data->igp_mmio + offset);
}

static u16 igp_read_short(struct gm965temp_data *data, unsigned long offset)
{
	return ioread16(data->igp_mmio + offset);
}

static u32 igp_read_int(struct gm965temp_data *data, unsigned long offset)
{
	return ioread32(data->igp_mmio + offset);
}

static void igp_write_short(struct gm965temp_data *data, unsigned long offset,
				u16 val)
{
	iowrite16(val, data->igp_mmio + offset);
}

static ssize_t show_label(struct device *dev,
		struct device_attribute *devattr,
		char *buf)
{
	return sprintf(buf, "GM965 IGP\n");
}


#define TSC1	0x1001
#define TSS1	0x1004
#define TR1	0x1006
#define TOF1	0x1007
#define RTR1	0x1008

#define G_TSC1	0xCD8	/*8 bits*/	
#define G_TSS	0xCDA   /*8 bits*/
#define TSTTP	0xCDC	/*32 bits*/

#define G_TSE	0x80

#define TSE	0x8000
#define TMOV	(0x01 << 10)
#define G_TMOV	(0x01 << 4)

#define RELT_MASK  0xFF000000	/*bits 31:24*/
#define HTPS_MASK  0xFF00	/*bits 15:8*/
#define CTPS_MASK  0xFF		/*bits 7:0*/

#define MAX_RETRIES  36

static struct gm965temp_data *gm965temp_update_device(struct device *dev)
{
	struct gm965temp_data *data = dev_get_drvdata(dev);
	unsigned short tsc1_val = 0;
#ifdef USE_RTR
	char  rtr1_val = 0;
	unsigned char  tof1_val = 0;
#else
	unsigned char  tr1_val = 0;
#endif
	unsigned short tss1_val = 0, tmov = 0;
	unsigned int tsttp_val = 0, htps_val = 0;
	char relt_val = 0;
	unsigned int tsc1 = 0, tss1 = 0, tse = 0;
	int temp_val = 0;
	int i = MAX_RETRIES;

	data->temp = 0x0;
	if (IS_GM) {
		tsc1 = TSC1;
		tss1 = TSS1;
		tse = TSE;
		tmov = TMOV;
	} else {
		tsc1 = G_TSC1;
		tss1 = G_TSS;
		tse = G_TSE;
		tmov = G_TMOV;
	}

	mutex_lock(&data->update_lock);
	
	if (IS_GM)
		tsc1_val = igp_read_short(data, tsc1);
	else
		tsc1_val = igp_read_byte(data, tsc1);

	if (!(tsc1_val & tse)) {
		igp_write_short(data, tsc1, tsc1_val|tse);
		if (IS_GM)
			tsc1_val = igp_read_short(data, tsc1);
		else
			tsc1_val = igp_read_byte(data, tsc1);
	} else {
		if (IS_GM)
			tss1_val = igp_read_short(data, tss1);
		else
			tss1_val = igp_read_byte(data, tss1);

		/* Wait for the thermal sensor read ready */
		while (!(tss1_val & tmov) && (0 < i--)) {
			mdelay(1);
			udelay(300);
			if (IS_GM)
				tss1_val = igp_read_short(data, tss1);
			else
				tss1_val = igp_read_byte(data, tss1);
		}

 		/* function to map register values to temperatures taken from
 		 * p. 358 http://www.intel.com/design/chipsets/datashts/313053.htm
 		 */
		if (tss1_val & tmov) {
			if (IS_GM) {
#ifdef USE_RTR
				rtr1_val = igp_read_byte(data, RTR1);
				tof1_val = igp_read_byte(data, TOF1);
#ifdef DEBUG
	 			printk(KERN_DEBUG "%s: read values RTR1: %i and TOF1: %i\n",
					DRVNAME, rtr1_val, tof1_val);
#endif
				rtr1_val += tof1_val;
				if ((rtr1_val != 0xFF) && (rtr1_val != 0x0)) {
					data->temp = (16*rtr1_val*rtr1_val
						- 11071*rtr1_val + 1610500)/10;
				}
#else
				tr1_val = igp_read_byte(data, TR1);
#ifdef DEBUG
				printk(KERN_DEBUG "%s: read value TR1: %i\n", DRVNAME, tr1_val);
#endif
				if ((tr1_val != 0xFF) && (tr1_val != 0x0)) {
					data->temp = (16*tr1_val*tr1_val
						- 11071*tr1_val + 1610500)/10;
				}
#endif
			} else {
				tsttp_val = igp_read_int(data, TSTTP);		
				htps_val = (tsttp_val & HTPS_MASK ) >> 8;
				relt_val = (tsttp_val & RELT_MASK) >> 24;
#ifdef DEBUG
 				printk(KERN_DEBUG "%s: read values RELT: %i, HTPS: %i and CTPS %i\n",
				DRVNAME, relt_val, htps_val, tsttp_val & CTPS_MASK);
#endif
				temp_val = htps_val + relt_val;
				data->temp = (16*temp_val*relt_val
					- 11071*temp_val + 1610500)/10;
			}
		}
	}

	mutex_unlock(&data->update_lock);

	return data;
}

static ssize_t show_temp(struct device *dev,
				struct device_attribute *devattr,
				char *buf)
{
	struct gm965temp_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	unsigned int temp = 0;

	if (attr->index == SHOW_TEMP) {
		data = gm965temp_update_device(dev);
		temp = data->temp;
		return sprintf(buf, "%d\n", temp);
	} else {
		unsigned int TjMax = 110;
		return sprintf(buf, "%d\n", TjMax*1000);
	}
}

static int __devinit gm965temp_hwmon_init(struct platform_device *pdev)
{
	int res = 0;
	struct gm965temp_data *data = platform_get_drvdata(pdev);

	res = device_create_file(&pdev->dev, &dev_attr_name);
	if (res)
		goto exit_remove;

	res = sysfs_create_group(&pdev->dev.kobj, &gm965temp_group);
	if (res)
		goto exit_dev;

	data->hwmon_dev = hwmon_device_register(&pdev->dev);
	if (IS_ERR(data->hwmon_dev)) {
		res = PTR_ERR(data->hwmon_dev);
		goto exit_remove;
	}

	return res;

exit_remove:
	sysfs_remove_group(&pdev->dev.kobj, &gm965temp_group);
exit_dev:
	device_remove_file(&pdev->dev, &dev_attr_name);

	return res;
}

static int __devinit gm965temp_add(void)
{
	int res = -ENODEV;

	/* only ever going to be one of these */
	igp_pdev = platform_device_alloc(DRVNAME, 0);
	if (!igp_pdev)
		return -ENOMEM;

	res = platform_device_add(igp_pdev);
	if (res)
		goto err;
	return 0;

err:
	platform_device_put(igp_pdev);
	return res;
}

static int __devinit gm965_find_registers(struct gm965temp_data *data,
					    unsigned long devid)
{
	struct pci_dev *pcidev = NULL;
	u32 val32 = 0x0UL;
	u64 val64 = 0x0ULL;
	int res = -ENODEV;

	pcidev = pci_get_device(PCI_VENDOR_ID_INTEL,
				devid,
				NULL);
	if (!pcidev) {
		printk(KERN_INFO "Try pci_get_device devid 0x%lx failed\n",
                                devid);
		return -ENODEV;
	}

	if (pci_read_config_dword(pcidev, MCHBAR_I965, &val32))
		goto out;

	if (!(val32 & 1))
		pci_write_config_dword(pcidev, MCHBAR_I965, val32 | 1);

	val64 = (u64)val32;
	if (pci_read_config_dword(pcidev, MCHBAR_I965 + 4, &val32))
		goto out;

	val64 |= (u64)val32 << 32;
	data->igp_base = val64 & MCHBAR_MASK;
	data->igp_len = 16*1024;

	data->chipset_id = devid;

	res = 0;
out:
	pci_dev_put(pcidev);
	return res;
}

static unsigned long chipset_ids[] = {
	PCI_DEVICE_ID_INTEL_82965GM,
	PCI_DEVICE_ID_INTEL_82965GME,
	PCI_DEVICE_ID_INTEL_GM45,
	PCI_DEVICE_ID_INTEL_Q35,
	PCI_DEVICE_ID_INTEL_G33,
	PCI_DEVICE_ID_INTEL_Q33,
	PCI_DEVICE_ID_INTEL_Q45,
	PCI_DEVICE_ID_INTEL_G45,
	PCI_DEVICE_ID_INTEL_G41,
	PCI_DEVICE_ID_INTEL_B43_BASE,
	PCI_DEVICE_ID_INTEL_B43_SOFT_SKU,
	0
};

static int __devinit gm965temp_probe(struct platform_device *pdev)
{
	struct gm965temp_data *data;
	struct resource *reso;
	int i;
	int res = -ENODEV;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	i = 0;
	do {
		res = gm965_find_registers(data, chipset_ids[i]);
		i++;
	} while (res && chipset_ids[i]);

	if (res)
		goto err;
	/* Set up resource regions */
	reso = request_mem_region(data->igp_base, data->igp_len, DRVNAME);
	if (!reso) {
		printk(KERN_ERR "request_mem_region failed\n");
		res = -EBUSY;
		goto err;
	}

	data->igp_mmio = ioremap_nocache(data->igp_base, data->igp_len);
	if (!data->igp_mmio) {
		printk(KERN_ERR "ioremap_nocache failed\n");
		res = -EBUSY;
		goto err_map_failed;
	}

	platform_set_drvdata(pdev, data);

	mutex_init(&data->update_lock);

	res = gm965temp_hwmon_init(pdev);
	if (res)
		goto err_init_failed;

	return res;

err_init_failed:
	iounmap(data->igp_mmio);
	platform_set_drvdata(pdev, NULL);
err_map_failed:
	release_mem_region(data->igp_base, data->igp_len);
err:
	kfree(data);
	return res;
}

static int __devexit gm965temp_remove(struct platform_device *pdev)
{
	struct gm965temp_data *data = platform_get_drvdata(pdev);
	hwmon_device_unregister(data->hwmon_dev);
	device_remove_file(&pdev->dev, &dev_attr_name);

	iounmap(data->igp_mmio);
	release_mem_region(data->igp_base, data->igp_len);
	platform_set_drvdata(pdev, NULL);
	kfree(data);
	return 0;
}

static struct platform_driver gm965temp_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = DRVNAME,
	},
	.probe = gm965temp_probe,
	.remove = __devexit_p(gm965temp_remove),
};

static int __init gm965temp_init(void)
{
	int res = platform_driver_register(&gm965temp_driver);
	if (res)
		return res;

	res = gm965temp_add();
	if (res)
		platform_driver_unregister(&gm965temp_driver);

	return res;
}

static void __exit gm965temp_exit(void)
{
	platform_device_unregister(igp_pdev);
	platform_driver_unregister(&gm965temp_driver);
}

MODULE_AUTHOR("Lu Zhihe");
MODULE_DESCRIPTION("Intel GM965 chipset IGP temperature sensor");
MODULE_LICENSE("GPL");

module_init(gm965temp_init);
module_exit(gm965temp_exit);

