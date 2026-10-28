#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/spi/spi.h>
#include <linux/types.h>


MODULE_LICENSE("Dual BSD/GPL");


struct dw_spi_chip {
	u8 poll_mode;	// 0 for contoller polling mode
	u8 type;	// SPI/SSP/Micrwire
	u8 enable_dma;
	void (*cs_control)(u32 command);
};


static struct dw_spi_chip spidev_slave_info[] = {
{
	//	slave 0 on bus 0
	.poll_mode = 1,
	.type = 0,
	.enable_dma = 0,
},
};


static struct spi_board_info spidev_slave_board_info_info[] __initdata = {
	{
		.modalias = "spidev", /* Name of spi_driver for this device */
		.max_speed_hz = 100000000, /* Run SSP as fast a possbile */
		.bus_num = 0, /* Framework bus number */
		.chip_select = 0, /* Framework chip select */
		//.platform_data = 0, /* No spi_driver specific config */
		.controller_data = &(spidev_slave_info[0]), /* Master chip config */
		.mode = 3,
	},
};


static int __init  spi_slave_init(void)
{
  spi_register_board_info(spidev_slave_board_info_info, 1);//wrong one sizeof(spidev_slave_board_info_info));
  return 0;
}
module_init(spi_slave_init);


static void __exit spi_slave_exit(void)
{
  printk("<0> Bye, cruel world\n");
}
module_exit(spi_slave_exit);
