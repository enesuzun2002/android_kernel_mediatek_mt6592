#include <linux/types.h>
#include <mach/mt_pm_ldo.h>
#include <cust_gyro.h>

/* ETAB5: as the stock kernel's cust_gyro_hw - BMG160 on i2c-2, direction 5. */
static struct gyro_hw cust_gyro_hw = {
    .addr = 0,
    .i2c_num = 2,
    .direction = 5,
    .power_id = MT65XX_POWER_NONE,  /*!< LDO is not used */
    .power_vol = VOL_DEFAULT,       /*!< LDO is not used */
    .firlen = 0,
};

struct gyro_hw *get_cust_gyro_hw(void)
{
    return &cust_gyro_hw;
}
