include $(srctree)/drivers/misc/mediatek/Makefile.custom


# In case the platform does NOT support this type of sensors
obj-y   +=  mtk_tpd.o
obj-y   +=  tpd_button.o
obj-y   +=  tpd_calibrate.o
obj-y   +=  tpd_debug.o
obj-y   +=  tpd_default.o
obj-y   +=  tpd_init.o
obj-y   +=  tpd_misc.o
obj-y   +=  tpd_setting.o

# Touch driver selection is per-project and exclusive: each of these
# drivers defines the same global symbols (thread, tpd_i2c_driver, ...),
# so exactly one may be built into the kernel. This tree now targets
# hulkbuster only, whose panel carries a FocalTech FT5526 (FT5x06 family),
# so ft5x0x is the selection here. A multi-project build should move this
# choice into Kconfig.
obj-y	+=  ft5x0x/


