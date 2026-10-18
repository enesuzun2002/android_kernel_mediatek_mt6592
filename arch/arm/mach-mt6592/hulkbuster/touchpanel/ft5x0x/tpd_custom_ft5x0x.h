/*
 * Touch panel configuration for the General Mobile ETAB5 (hulkbuster).
 *
 * Touch controller: FocalTech FT5526 (chip id 0x59 at reg 0xA3 - see panel
 * datasheet OSD100T2587-53TS rev A.1, TP drawing note 7). It behaves as an
 * FT5x06 family part; the reference (prebuilt 3.10.72) kernel logs
 * "[tpd info:5x06] ... addr:0x38" on probe, which is what this
 * configuration reproduces:
 *
 *   - i2c address 0x38 on the panel's i2c bus
 *   - interrupt through EINT2 (GPIO2, falling edge, no debounce)
 *   - panel resolution 1920 x 1200 (matches the LCM: "TPD_RES_X = 1920,
 *     TPD_RES_Y = 1200" in the reference boot log)
 *   - no hardware keys reported by the touch controller (the device's
 *     keys come from the MTK keypad/mt_devs path, not the touch IC)
 *
 * Board-level wiring (EINT2/GPIO2, RST GPIO14) is carried over from the
 * p55n project template and is the first thing to re-verify if touch does
 * not respond on hardware. The panel module itself has no touch reset
 * line per its pinout, but the FT5x06 driver performs a reset sequence,
 * so a GPIO is still configured.
 */

#ifndef __TPD_CUSTOM_FT5X0X_H__
#define __TPD_CUSTOM_FT5X0X_H__

/* FocalTech I2C address as used in MTK trees: 8-bit form (0x38 << 1). */
#define FT5X0X_I2C_ADDR            0x70

/* Panel resolution */
#define TPD_RES_X                  1200
#define TPD_RES_Y                  800

/* The MTK tpd framework swaps the axes for landscape panels configured
 * via TPD_ROTATE_90; with RES 1200x800 + rotate 90 the controller space
 * (portrait) is transformed into the 1920x1200 landscape framebuffer. */
#define TPD_ROTATE_90

/* No hardware keys through the touch IC on this device. */
#define TPD_KEY_COUNT              2
#define TPD_KEYS                   {KEY_BACK, KEY_HOME}
#define TPD_KEYS_DIM               {{540, 730, 80}, {600, 730, 80}}

/* Calibration matrix in the style used by the other FocalTech projects
 * in this tree (values are the usual full-screen affine mapping). */
#define TPD_HAVE_CALIBRATION
#define TPD_CALIBRATION_MATRIX_ROTATION_NORMAL  {1600, 0, 0, 0, 1600, 0, 0, 0}
#define TPD_CALIBRATION_MATRIX_ROTATION_FACTORY {1600, 0, 0, 0, 1600, 0, 0, 0}

/* Power: touch VDD is on the PMIC rail VGP3 (1.8 V) on this platform. */
#define TPD_POWER_SOURCE_CUSTOM    MT6322_POWER_LDO_VGP3
#define TPD_POWER_SOURCE_1800      MT6322_POWER_LDO_VGP3

/* Charging noise workaround (used on many 3.10 boards with resistive
 * charge-pump touch panels). */
#define TPD_SOLVE_CHARGING_ISSUE  1

/* FocalTech supplier variants - value 0 selects the default ft5x06 path. */
#define TPD_SUPPLIER_0             0x05
#define TPD_SUPPLIER_1             0x08
#define TPD_SUPPLIER_2             0x0a
#define TPD_SUPPLIER_3             0x0b

/* Types used by the driver code */
#define TPD_BYTE                   u8
#define TPD_WORD                   u16
#define TPD_DWRD                   u32
#define TPD_BOOL                   kal_bool

/* Debug hooks (no-op builds: define TPD_DEBUG to enable prints) */
#define TPD_DEBUG                  0
#define TPD_DMESG(fmt, ...)        printk(fmt, ##__VA_ARGS__)

#endif /* __TPD_CUSTOM_FT5X0X_H__ */