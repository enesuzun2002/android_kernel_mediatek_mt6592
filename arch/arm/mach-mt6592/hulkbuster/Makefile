
include $(srctree)/drivers/misc/mediatek/Makefile.custom

# Module project related part

obj-y	+=  alsps/cm36283/
obj-y   +=  accelerometer/bma056/
# ETAB5 stock sensors: BMG160 gyro @0x68, BMM050 compass @0x12, both i2c-2
obj-y   +=  gyroscope/bmg160/
obj-y   +=  magnetometer/bmm050/

ifneq ($(CONFIG_ARCH_MTK_PROJECT),)
ARCH_MTK_PROJECT := $(shell echo $(CONFIG_ARCH_MTK_PROJECT))
obj-y	+= core/
obj-y	+= dct/dct/
endif

obj-y	+=  camera/camera/
obj-y   +=  flashlight/constant_flashlight/
obj-y	+=  vibrator/
obj-y	+=  leds/
obj-y	+=  keypad/
obj-y	+=  sound/
obj-y	+=  accdet/

obj-y	+=  hdmi/

