/*
 * Copyright (c) 2011 Jamie Iles
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * All enquiries to support@picochip.com
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/basic_mmio_gpio.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>

#define INT_EN_REG_OFFS	0x30
#define INT_MASK_REG_OFFS	0x34
#define INT_TYPE_REG_OFFS	0x38
#define INT_POLARITY_REG_OFFS	0x3c
#define INT_STATUS_REG_OFFS	0x40
#define EOI_REG_OFFS		0x4c
#define EXT_PORT_OFFS		0x50
#define DW_MAX_BANK		4

#define GA_REG_WORD32_READ(a, pv)	(*pv = __raw_readl(IOMEM(a)))
#define GA_REG_WORD32_WRITE(a, v)	__raw_writel(v, IOMEM(a))

unsigned int gpio_irq_status1 = 0;


#define MEMMAP_APBPERIF_REG_BASE	0xF7E80000
#define MEMMAP_CHIP_CTRL_REG_BASE	0xF7EA0000
#define MEMMAP_SM_REG_BASE		0xF7F80000

#ifdef CONFIG_BERLIN2CDP
#define MEMMAP_CA7_REG_BASE             0xF7920000
#endif

#define APB_GPIO_INST0_BASE		(MEMMAP_APBPERIF_REG_BASE + 0x0400)
#define APB_GPIO_INST1_BASE		(MEMMAP_APBPERIF_REG_BASE + 0x0800)
#define APB_GPIO_INST2_BASE		(MEMMAP_APBPERIF_REG_BASE + 0x0C00)
#define APB_GPIO_INST3_BASE		(MEMMAP_APBPERIF_REG_BASE + 0x1000)
#define APB_I2C_INST0_BASE		(MEMMAP_APBPERIF_REG_BASE + 0x1400)
#define APB_I2C_INST1_BASE		(MEMMAP_APBPERIF_REG_BASE + 0x1800)

#define APB_WDT_INST0_BASE              (MEMMAP_APBPERIF_REG_BASE + 0x2000)
#define APB_WDT_INST1_BASE              (MEMMAP_APBPERIF_REG_BASE + 0x2400)
#define APB_WDT_INST2_BASE              (MEMMAP_APBPERIF_REG_BASE + 0x2800)

#define SOC_SM_APB_REG_BASE		(MEMMAP_SM_REG_BASE + 0x40000)

#define SM_APB_GPIO1_BASE		(SOC_SM_APB_REG_BASE + 0x5000)
#define SM_APB_I2C0_BASE		(SOC_SM_APB_REG_BASE + 0x7000)
#define SM_APB_I2C1_BASE		(SOC_SM_APB_REG_BASE + 0x8000)
#define SM_APB_GPIO0_BASE		(SOC_SM_APB_REG_BASE + 0xC000)
#define SM_APB_GPIO_BASE		SM_APB_GPIO0_BASE
#define APB_I2C_INST2_BASE              SM_APB_I2C0_BASE
#define APB_I2C_INST3_BASE              SM_APB_I2C1_BASE

#define SM_SYS_CTRL_REG_BASE            (SOC_SM_APB_REG_BASE + 0xD000)


#define APB_GPIO_SWPORTA_DR		0x00
#define APB_GPIO_SWPORTA_DDR		0x04
#define APB_GPIO_PORTA_CTL		0x08
#define APB_GPIO_SWPORTB_DR		0x0c
#define APB_GPIO_SWPORTB_DDR		0x10
#define APB_GPIO_PORTB_CTL		0x14
#define APB_GPIO_SWPORTC_DR		0x18
#define APB_GPIO_SWPORTC_DDR		0x1c
#define APB_GPIO_PORTC_CTL		0x20
#define APB_GPIO_SWPORTD_DR		0x24
#define APB_GPIO_SWPORTD_DDR		0x28
#define APB_GPIO_PORTD_CTL		0x2c
#define APB_GPIO_INTEN			0x30
#define APB_GPIO_INTMASK		0x34
#define APB_GPIO_INTTYPE_LEVEL		0x38
#define APB_GPIO_INT_POLARITY		0x3c
#define APB_GPIO_INTSTATUS		0x40
#define APB_GPIO_RAWINTSTATUS		0x44
#define APB_GPIO_DEBOUNCE		0x48
#define APB_GPIO_PORTA_EOI		0x4c
#define APB_GPIO_EXT_PORTA		0x50
#define APB_GPIO_EXT_PORTB		0x54
#define APB_GPIO_EXT_PORTC		0x58
#define APB_GPIO_EXT_PORTD		0x5c
#define APB_GPIO_LS_SYNC		0x60
#define APB_GPIO_ID_CODE		0x64
#define APB_GPIO_RESERVED		0x68
#define APB_GPIO_COMP_VERSION		0x6c

#define PORT_DDR_IN	0
#define PORT_DDR_OUT	1
#define NO_REG	0xFFFF


struct dwapb_gpio;

struct dwapb_gpio_bank {
	struct bgpio_chip	bgc;
	bool			is_registered;
	struct dwapb_gpio	*gpio;
	struct irq_domain *domain;
};

struct dwapb_gpio {
	struct device_node	*of_node;
	struct	device		*dev;
	void __iomem		*regs;
	struct dwapb_gpio_bank	*banks;
	unsigned int		nr_banks;
	struct irq_chip_generic	*irq_gc;
	unsigned long		toggle_edge;
#ifdef CONFIG_PM_SLEEP
	u32			dr[DW_MAX_BANK];
	u32			ddr[DW_MAX_BANK];
	u32			int_type;
	u32			int_en;
	u32			int_polarity;
#endif
};


static void dwapb_gpio_set_value(struct gpio_chip *chip, unsigned offset,
								int value);
static int dwapb_gpio_get_value(struct gpio_chip *chip, unsigned offset);

static int dwapb_gpio_direction_output(struct gpio_chip *chip, unsigned offset,
								int value);
static int dwapb_gpio_direction_input(struct gpio_chip *chip, unsigned offset);


static void dwapb_gpio_set_value(struct gpio_chip *chip, unsigned offset,
								int value)
{

	int reg_ddr, reg_dr, gpio_port;
	int ddr, dr;
	int port = offset;

	if((port >= 0) && (port < 32))
	{
		reg_ddr = APB_GPIO_INST0_BASE + APB_GPIO_SWPORTA_DDR;
		reg_dr = APB_GPIO_INST0_BASE + APB_GPIO_SWPORTA_DR;
	} 
	else if ((port >= 32) && (port < 64))
	{
		reg_ddr = APB_GPIO_INST1_BASE + APB_GPIO_SWPORTA_DDR;
		reg_dr = APB_GPIO_INST1_BASE + APB_GPIO_SWPORTA_DR;
		port -= 32;
	} 
	else if ((port >= 64) && (port < 96))
	{
		reg_ddr = APB_GPIO_INST2_BASE + APB_GPIO_SWPORTA_DDR;
		reg_dr = APB_GPIO_INST2_BASE + APB_GPIO_SWPORTA_DR;
		port -= 64;
	} 
	else if ((port >= 96) && (port < 128))
	{
		reg_ddr = APB_GPIO_INST3_BASE + APB_GPIO_SWPORTA_DDR;
		reg_dr = APB_GPIO_INST3_BASE + APB_GPIO_SWPORTA_DR;
		port -= 96;

	} else
		return;

	//GPIO_PortLock(gpio_port);

	/* set port value */
	GA_REG_WORD32_READ(reg_dr, &dr);
	if (value){
		dr |= (1<<port);
	} else {
		dr &= ~(1<<port);
	}
	GA_REG_WORD32_WRITE(reg_dr, dr);

	//GPIO_PortUnlock(gpio_port);

	return;

}


static int dwapb_gpio_get_value(struct gpio_chip *chip, unsigned offset)
{

	int reg_ddr, reg_ext, gpio_port ;
	int ddr, ext, value;
	int port = offset;

	if((port >= 0) && (port < 32))
	{
		reg_ddr = APB_GPIO_INST0_BASE + APB_GPIO_SWPORTA_DDR;
		reg_ext = APB_GPIO_INST0_BASE + APB_GPIO_EXT_PORTA;
	} 
	else if ((port >= 32) && (port < 64))
	{
		reg_ddr = APB_GPIO_INST1_BASE + APB_GPIO_SWPORTA_DDR;
		reg_ext = APB_GPIO_INST1_BASE + APB_GPIO_EXT_PORTA;
		port -= 32;
	} 
	else if ((port >= 64) && (port < 96))
	{
		reg_ddr = APB_GPIO_INST2_BASE + APB_GPIO_SWPORTA_DDR;
		reg_ext = APB_GPIO_INST2_BASE + APB_GPIO_EXT_PORTA;
		port -= 64;
	} 
	else if ((port >= 96) && (port < 128))
	{
		reg_ddr = APB_GPIO_INST3_BASE + APB_GPIO_SWPORTA_DDR;
		reg_ext = APB_GPIO_INST3_BASE + APB_GPIO_EXT_PORTA;
		port -= 96;
	} 
	else
		return -1;
	
	//	GPIO_PortLock(gpio_port);
	
		/* set port to input mode */
	//	GA_REG_WORD32_READ(reg_ddr, &ddr);
	//	ddr &= ~(1<<port);
	//	GA_REG_WORD32_WRITE(reg_ddr, ddr);
	
		/* get port value */
		GA_REG_WORD32_READ(reg_ext, &ext);
		if (ext & (1<<port))
			value = 1;
		else
			value = 0;
	
	//	GPIO_PortUnlock(gpio_port);
	
		return value;

}



static int dwapb_gpio_direction_output(struct gpio_chip *chip, unsigned offset,
								int value)
{

	int reg_ddr, reg_ctl, gpio_port;
	int ddr, ctl;
	int port = offset;

	if((port >= 0) && (port < 32))
	{
		reg_ddr = APB_GPIO_INST0_BASE + APB_GPIO_SWPORTA_DDR;
		reg_ctl = APB_GPIO_INST0_BASE + APB_GPIO_PORTA_CTL;
	} 
	else if ((port >= 32) && (port < 64))
	{
		reg_ddr = APB_GPIO_INST1_BASE + APB_GPIO_SWPORTA_DDR;
		reg_ctl = APB_GPIO_INST1_BASE + APB_GPIO_PORTA_CTL;
		port -= 32;
	} 
	else
		return -1;

//	GPIO_PortLock(gpio_port);
	/* set port to output mode */
	GA_REG_WORD32_READ(reg_ddr, &ddr);
	ddr |= (1 << port);
	GA_REG_WORD32_WRITE(reg_ddr, ddr);

//	GPIO_PortUnlock(gpio_port);

	return 0;
}


static int dwapb_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{

	int reg_ddr, reg_ctl;
	int ddr, ctl;
	int port = offset;

	if((port >= 0) && (port < 32))
	{
		reg_ddr = APB_GPIO_INST0_BASE + APB_GPIO_SWPORTA_DDR;
		reg_ctl = APB_GPIO_INST0_BASE + APB_GPIO_PORTA_CTL;
	} 
	else if ((port >= 32) && (port < 64))
	{
		reg_ddr = APB_GPIO_INST1_BASE + APB_GPIO_SWPORTA_DDR;
		reg_ctl = APB_GPIO_INST1_BASE + APB_GPIO_PORTA_CTL;
		port -= 32;
	} 
	else
		return -1;

//	GPIO_PortLock(gpio_port);
	/* set port to output mode */
	GA_REG_WORD32_READ(reg_ddr, &ddr);
	ddr &= ~(1 << port);
	GA_REG_WORD32_WRITE(reg_ddr, ddr);

//	GPIO_PortUnlock(gpio_port);

	return 0;

}

static unsigned int dwapb_gpio_nr_banks(struct device_node *of_node)
{
	unsigned int nr_banks = 0;
	struct device_node *np;

	for_each_child_of_node(of_node, np)
		++nr_banks;

	return nr_banks;
}

static int dwapb_gpio_to_irq(struct gpio_chip *gc, unsigned offset)
{
	struct dwapb_gpio_bank *bank;

	bank = container_of((struct bgpio_chip *)gc, struct dwapb_gpio_bank, bgc);
	return irq_find_mapping(bank->domain, offset);
}

static void dwapb_toggle_trigger(struct dwapb_gpio *gpio, unsigned int offs)
{
	u32 v = readl(gpio->regs + INT_POLARITY_REG_OFFS);
	u32 in = readl(gpio->regs + EXT_PORT_OFFS);

	if (in & BIT(offs))
		v &= ~BIT(offs);
	else
		v |= BIT(offs);

	writel(v, gpio->regs + INT_POLARITY_REG_OFFS);
}

static struct dwapb_gpio_bank* dwapb_irq_get_bank(struct dwapb_gpio *gpio, u32 irq)
{
	return &gpio->banks[0];
}

static void dwapb_irq_handler(u32 irq, struct irq_desc *desc)
{
	struct dwapb_gpio *gpio;
	struct dwapb_gpio_bank *bank;
	u32 irq_status;

	gpio = irq_get_handler_data(irq);
	bank = dwapb_irq_get_bank(gpio, irq);
	irq_status = readl(gpio->regs + INT_STATUS_REG_OFFS);

	while (irq_status) {
		int irqoffset = fls(irq_status) - 1;
		int irq = irq_find_mapping(bank->domain, irqoffset);

		generic_handle_irq(irq);
		irq_status &= ~(1 << irqoffset);
		if (gpio->toggle_edge & BIT(irqoffset))
			dwapb_toggle_trigger(gpio, irqoffset);
	}
}

static void dwapb_irq_enable(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct dwapb_gpio *gpio = gc->private;

	u32 val = readl(gpio->regs + INT_EN_REG_OFFS);
	val |= 1 << d->hwirq;
	writel(val, gpio->regs + INT_EN_REG_OFFS);
}

static void dwapb_irq_disable(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct dwapb_gpio *gpio = gc->private;

	u32 val = readl(gpio->regs + INT_EN_REG_OFFS);
	val &= ~(1 << d->hwirq);
	writel(val, gpio->regs + INT_EN_REG_OFFS);
}

static int dwapb_irq_set_type(struct irq_data *d, u32 type)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct dwapb_gpio *gpio = gc->private;
	int bit = d->hwirq;
	unsigned long level, polarity;

	if (type & ~(IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING |
		     IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW))
		return -EINVAL;

	level = readl(gpio->regs + INT_TYPE_REG_OFFS);
	polarity = readl(gpio->regs + INT_POLARITY_REG_OFFS);

	gpio->toggle_edge &= ~BIT(bit);
	if (type == IRQ_TYPE_EDGE_BOTH) {
		gpio->toggle_edge |= BIT(bit);
		level |= (1 << bit);
		dwapb_toggle_trigger(gpio, bit);
		writel(level, gpio->regs + INT_TYPE_REG_OFFS);
		return 0;
	} else if (type == IRQ_TYPE_EDGE_RISING) {
		level |= (1 << bit);
		polarity |= (1 << bit);
	} else if (type == IRQ_TYPE_EDGE_FALLING) {
		level |= (1 << bit);
		polarity &= ~(1 << bit);
	} else if (type == IRQ_TYPE_LEVEL_HIGH) {
		level &= ~(1 << bit);
		polarity |= (1 << bit);
	} else if (type == IRQ_TYPE_LEVEL_LOW) {
		level &= ~(1 << bit);
		polarity &= ~(1 << bit);
	} else {
		return -EINVAL;
	}

	writel(level, gpio->regs + INT_TYPE_REG_OFFS);
	writel(polarity, gpio->regs + INT_POLARITY_REG_OFFS);

	return 0;
}

static int dwapb_create_irqchip(struct dwapb_gpio *gpio,
				struct dwapb_gpio_bank *bank,
				unsigned int irq_base)
{
	struct irq_chip_type *ct;

	gpio->irq_gc = irq_alloc_generic_chip("gpio-dwapb", 1, irq_base,
					      gpio->regs, handle_level_irq);
	if (!gpio->irq_gc)
		return -EIO;

	bank->domain->of_node = of_node_get(bank->bgc.gc.of_node);
	gpio->irq_gc->private = gpio;
	ct = gpio->irq_gc->chip_types;
	ct->chip.irq_ack = irq_gc_ack_set_bit;
	ct->chip.irq_mask = irq_gc_mask_set_bit;
	ct->chip.irq_unmask = irq_gc_mask_clr_bit;
	ct->chip.irq_set_type = dwapb_irq_set_type;
	ct->chip.irq_enable = dwapb_irq_enable;
	ct->chip.irq_disable = dwapb_irq_disable;
	ct->regs.ack = EOI_REG_OFFS;
	ct->regs.mask = INT_MASK_REG_OFFS;
	irq_setup_generic_chip(gpio->irq_gc, IRQ_MSK(bank->bgc.gc.ngpio),
			       IRQ_GC_INIT_NESTED_LOCK, IRQ_NOREQUEST, 0);

	return 0;
}

static int dwapb_configure_irqs(struct dwapb_gpio *gpio,
				struct dwapb_gpio_bank *bank)
{
	unsigned int m, irq, ngpio = bank->bgc.gc.ngpio;
	int irq_base;

	for (m = 0; m < ngpio; ++m) {
		irq = irq_of_parse_and_map(bank->bgc.gc.of_node, m);
		if (!irq && m == 0) {
			dev_warn(gpio->dev, "no irq for bank %s\n",
				 bank->bgc.gc.of_node->full_name);
			return -ENXIO;
		} else if (!irq) {
			break;
		}

		irq_set_chained_handler(irq, dwapb_irq_handler);
		irq_set_handler_data(irq, gpio);
	}

	bank->bgc.gc.to_irq = dwapb_gpio_to_irq;

	irq_base = irq_alloc_descs(-1, 0, ngpio, NUMA_NO_NODE);
	if (irq_base < 0)
		return irq_base;

	/* only support irq in the first bank */
	bank->domain = irq_domain_add_legacy(gpio->of_node,
					   gpio->banks->bgc.gc.ngpio,
					   irq_base, 0,
					   &irq_domain_simple_ops, NULL);

	if (dwapb_create_irqchip(gpio, bank, irq_base))
		goto out_free_descs;

	return 0;

out_free_descs:
	irq_free_descs(irq_base, ngpio);

	return -EIO;
}

static int dwapb_gpio_add_bank(struct dwapb_gpio *gpio,
			       struct device_node *bank_np,
			       unsigned int offs)
{
	struct dwapb_gpio_bank *bank;
	u32 bank_idx, ngpio, base_gpio;
	int err;

	if (of_property_read_u32(bank_np, "reg", &bank_idx)) {
		dev_err(gpio->dev, "invalid bank index for %s\n",
			bank_np->full_name);
		return -EINVAL;
	}
	bank = &gpio->banks[offs];
	bank->gpio = gpio;

	if (of_property_read_u32(bank_np, "base-gpio", &base_gpio)) {
		dev_err(gpio->dev, "failed to get base gpio for %s\n",
			bank_np->full_name);
		return -EINVAL;
	}
	
	if (of_property_read_u32(bank_np, "nr-gpio", &ngpio)) {
		dev_err(gpio->dev, "failed to get number of gpios for %s\n",
			bank_np->full_name);
		return -EINVAL;
	}

	err = bgpio_init(&bank->bgc, gpio->dev, 4,
			gpio->regs + 0x50 + (bank_idx * 0x4),
			gpio->regs + 0x00 + (bank_idx * 0xc),
			NULL, gpio->regs + 0x04 + (bank_idx * 0xc), NULL,
			false);

	if (err) {
		dev_err(gpio->dev, "failed to init gpio chip for %s\n",
			bank_np->full_name);
		return err;
	}

	bank->bgc.gc.base = base_gpio;
	bank->bgc.gc.ngpio = ngpio;
	bank->bgc.gc.of_node = bank_np;
	bank->bgc.dir = bank->bgc.read_reg(bank->bgc.reg_dir);
	bank->bgc.data = bank->bgc.read_reg(bank->bgc.reg_set);
	bank->bgc.gc.direction_input = dwapb_gpio_direction_input;
	bank->bgc.gc.direction_output = dwapb_gpio_direction_output;
	bank->bgc.gc.get = dwapb_gpio_get_value;
	bank->bgc.gc.set = dwapb_gpio_set_value;

	/*
	 * Only bank A can provide interrupts in all configurations of the IP.
	 */
	if (bank_idx == 0 && of_get_property(bank_np, "interrupt-controller", NULL))
		dwapb_configure_irqs(gpio, bank);

	err = gpiochip_add(&bank->bgc.gc);
	if (err)
		dev_err(gpio->dev, "failed to register gpiochip for %s\n",
			bank_np->full_name);
	else
		bank->is_registered = true;

	return err;
}

static void dwapb_gpio_unregister(struct dwapb_gpio *gpio)
{
	unsigned int m;

	for (m = 0; m < gpio->nr_banks; ++m)
		if (gpio->banks[m].is_registered)
			WARN_ON(gpiochip_remove(&gpio->banks[m].bgc.gc));
	of_node_put(gpio->of_node);
}

#ifdef CONFIG_PM_SLEEP
static int dwapb_gpio_resume(struct device *dev)
{
	unsigned long flags;
	struct dwapb_gpio *gpio = (struct dwapb_gpio *)dev_get_drvdata(dev);
	int b;
	local_irq_save(flags);
	for (b = 0; b < gpio->nr_banks; b++) {
		writel(gpio->ddr[b], gpio->regs + 0x04 + (b * 0xc));
		writel(gpio->dr[b], gpio->regs + 0x00 + (b * 0xc));
	}
	writel(gpio->int_type, gpio->regs + INT_TYPE_REG_OFFS);
	writel(gpio->int_polarity, gpio->regs + INT_POLARITY_REG_OFFS);
	writel(gpio->int_en, gpio->regs + INT_EN_REG_OFFS);
	local_irq_restore(flags);
	return 0;
}
static int dwapb_gpio_suspend(struct device *dev)
{
	unsigned long flags;
	struct dwapb_gpio *gpio = (struct dwapb_gpio *)dev_get_drvdata(dev);
	int b;
	local_irq_save(flags);
	for (b = 0; b < gpio->nr_banks; b++) {
		gpio->dr[b] = readl(gpio->regs + 0x00 + (b * 0xc));
		gpio->ddr[b] = readl(gpio->regs + 0x04 + (b * 0xc));
	}
	gpio->int_type = readl(gpio->regs + INT_TYPE_REG_OFFS);
	gpio->int_polarity = readl(gpio->regs + INT_POLARITY_REG_OFFS);
	gpio->int_en = readl(gpio->regs + INT_EN_REG_OFFS);
	local_irq_restore(flags);
	return 0;
}
#endif
static const struct dev_pm_ops dwapb_gpio_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwapb_gpio_suspend, dwapb_gpio_resume)
};
static int __devinit dwapb_gpio_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct dwapb_gpio *gpio;
	struct device_node *np;
	const char *dev_name;
	int err;
	unsigned int offs = 0;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;
	gpio->dev = &pdev->dev;

	gpio->nr_banks = dwapb_gpio_nr_banks(pdev->dev.of_node);

	if (!gpio->nr_banks || (gpio->nr_banks > DW_MAX_BANK))
		return -EINVAL;
	gpio->banks = devm_kzalloc(&pdev->dev, gpio->nr_banks *
			   sizeof(*gpio->banks), GFP_KERNEL);
	if (!gpio->banks)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get iomem\n");
		return -ENXIO;
	}

	gpio->regs = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!gpio->regs)
		return -ENOMEM;

	gpio->of_node = of_node_get(pdev->dev.of_node);
	dev_name = of_get_property(gpio->of_node, "dev_name", NULL);
	if (dev_name)
		dev_set_name(gpio->dev, dev_name);
	for_each_child_of_node(pdev->dev.of_node, np) {
		err = dwapb_gpio_add_bank(gpio, np, offs++);
		if (err)
			goto out_unregister;
	}

	platform_set_drvdata(pdev, gpio);
	return 0;

out_unregister:
	dwapb_gpio_unregister(gpio);

	return err;
}

static const struct of_device_id dwapb_of_match_table[] = {
	{ .compatible = "snps,dw-apb-gpio" },
	{ /* Sentinel */ }
};

static struct platform_driver dwapb_gpio_driver = {
	.driver		= {
		.name	= "gpio-dwapb",
		.owner	= THIS_MODULE,
		.pm	= &dwapb_gpio_pm_ops,
		.of_match_table = dwapb_of_match_table,
	},
	.probe		= dwapb_gpio_probe,
};

static int __init dwapb_gpio_init(void)
{
	return platform_driver_register(&dwapb_gpio_driver);
}
postcore_initcall(dwapb_gpio_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jamie Iles");
MODULE_DESCRIPTION("DesignWare APB GPIO driver");
