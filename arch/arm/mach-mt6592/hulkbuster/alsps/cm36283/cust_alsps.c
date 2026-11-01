#include <linux/types.h>
#include <mach/mt_pm_ldo.h>
#include <cust_alsps.h>

static struct alsps_hw cust_alsps_hw = {
	.i2c_num    = 2,
	.polling_mode_ps =0,
	.polling_mode_als =1,
	.power_id   = MT65XX_POWER_NONE,    /*LDO is not used*/
	.power_vol  = VOL_DEFAULT,          /*LDO is not used*/
	//.i2c_addr   = {0x0C, 0x48, 0x78, 0x00},
	//.als_level  = { 0,  1,  1,   7,  15,  15,  100, 1000, 2000,  3000,  6000, 10000, 14000, 18000, 20000},
    /* ETAB5: als_level/als_value/ps thresholds from the stock kernel cust_alsps_hw */
    .als_level  = { 0, 45, 45, 45, 303, 303, 303, 1305, 1305, 2699, 2699, 7622, 38416, 46881, 65535},
    .als_value  = { 0, 2, 2, 2, 12, 12, 12, 54, 54, 111, 111, 313, 1575, 1922, 2687, 10240},
    .i2c_addr   = {0x60, 0x48, 0x78, 0x00},
    .ps_threshold = 3,
    .ps_threshold_high = 31,
    .ps_threshold_low = 21,
};
struct alsps_hw *get_cust_alsps_hw(void) {
    return &cust_alsps_hw;
}

