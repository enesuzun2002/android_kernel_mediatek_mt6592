#include <linux/types.h>
#include <mach/mt_pm_ldo.h>
#include <cust_mag.h>

/* ETAB5: as the stock kernel's cust_mag_hw - BMM050 on i2c-2, direction 5. */
static struct mag_hw cust_mag_hw = {
    .i2c_num = 2,
    .direction = 5,
    .power_id = MT65XX_POWER_NONE,  /*!< LDO is not used */
    .power_vol = VOL_DEFAULT,       /*!< LDO is not used */
};

struct mag_hw *get_cust_mag_hw(void)
{
    return &cust_mag_hw;
}
