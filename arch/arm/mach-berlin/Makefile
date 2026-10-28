obj-y 	:= berlin.o irq.o time.o gpio.o wdt_reset.o hal_dhub.o
obj-$(CONFIG_PM_OPP) += opp.o
obj-$(CONFIG_SMP) += platsmp.o headsmp.o
obj-$(CONFIG_HOTPLUG_CPU) += hotplug.o

ifeq ($(CONFIG_BERLIN_PE)y, y)
obj-$(CONFIG_BERLIN_SHM) += modules/shm_amp/
else
obj-$(CONFIG_BERLIN_SHM) += modules/shm/
endif

obj-$(CONFIG_BERLIN_CC) += modules/cc/
obj-$(CONFIG_BERLIN_GPIO) += modules/gpio/
obj-$(CONFIG_BERLIN_I2C) += modules/i2c/
obj-$(CONFIG_BERLIN_PXA3XX_NFC) += modules/nfc/
obj-$(CONFIG_BERLIN_SDIO_WLAN_8787) += modules/wlan_sd8787/
obj-$(CONFIG_BERLIN_SDIO_WLAN_8797) += modules/wlan_sd8797/
obj-$(CONFIG_BERLIN_SDIO_WLAN_8801) += modules/wlan_sd8801/
obj-$(CONFIG_BERLIN_SDIO_WLAN_8887) += modules/wlan_sd8887/
obj-$(CONFIG_BERLIN_SDIO_BT_8887) += modules/bt_sd8887/
#obj-$(CONFIG_BERLIN_PE) += modules/amp/
#obj-$(CONFIG_BERLIN_FASTLOGO) += modules/fastlogo/
obj-$(CONFIG_BERLIN_GPU) += modules/gpu/
obj-$(CONFIG_BERLIN_GPU3D) += modules/gpu3D/
#obj-$(CONFIG_BERLIN_PE) += modules/pe/
obj-$(CONFIG_BERLIN_FASTLOGO) += modules/fastlogo/
obj-$(CONFIG_BERLIN_PWM) += modules/pwm/

ccflags-y       += -I$(srctree)/arch/arm/mach-berlin/include/mach
