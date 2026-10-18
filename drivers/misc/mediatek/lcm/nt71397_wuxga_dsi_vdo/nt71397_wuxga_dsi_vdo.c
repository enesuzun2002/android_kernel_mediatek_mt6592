/*
 * LCM driver for the OSD Displays OSD100T2587-53TS 10.1" WUXGA module.
 *
 * Module characteristics (datasheet OSD100T2587-53TS rev A.1, 2016-07-11):
 *   - 1920 x 1200 IPS, normally black, RGB888
 *   - MIPI DSI, video mode, 4 data lanes, no RAM on the module
 *   - Non-burst transmission with sync start/end (pulse mode)
 *   - Horizontal: HSA 20 + HBP 52 + 1920 + HFP 152 = 2144 dotclk
 *     Vertical:   VSA 6  + VBP 48 + 1200 + VFP 24  = 1278 lines
 *     (dotclk 164.4 MHz @ 60 Hz typical; the ETAB5 loader asks for 55.71 Hz,
 *      see "fps=" in the kernel command line)
 *   - Touch: FocalTech FT5526 on the same flex (i2c 0x38, handled by ft5x0x)
 *
 * The DSI receiver bridge on the module is a Novatek NT71397: a fixed
 * function DSI-to-RGB converter. It re-drives the panel from the video
 * stream, so no RAM/command-stream driver is needed - only the standard
 * DSI sleep-out/display-on packets. If a future revision of the module
 * needs bridge register writes, the init table below is the place.
 *
 * The module has no reset or enable pin (see pin assignment section 5.1:
 * T_GND / VCC / MIPI / LED cathodes only), so no board GPIO handling is
 * needed here. The backlight runs through the MTK BLS PWM path which the
 * lcm_util helpers already own.
 *
 * Selected automatically at boot: the bootloader passes
 *   lcm=0-nt71397_wuxga_dsi_vdo
 * in the kernel command line and mtkfb matches it against this driver's
 * name, so nothing else has to be configured.
 */

#ifndef BUILD_LK
#include <linux/string.h>
#endif
#include "lcm_drv.h"

#ifdef BUILD_LK
    #include <platform/disp_drv_platform.h>
#elif defined(BUILD_UBOOT)
    #include <asm/arch/mt_gpio.h>
    #include <platform/mt_pmic.h>
#else
    #include <linux/delay.h>
    #include <mach/mt_gpio.h>
    #include <mach/mt_pm_ldo.h>
#endif

#ifdef BUILD_LK
    #define LCD_DEBUG(fmt)  dprintf(CRITICAL,fmt)
#else
    #define LCD_DEBUG(fmt)  printk(fmt)
#endif

// ---------------------------------------------------------------------------
//  Local Constants
// ---------------------------------------------------------------------------
#define FRAME_WIDTH                                   (1920)
#define FRAME_HEIGHT                                  (1200)

// ---------------------------------------------------------------------------
//  Local Variables
// ---------------------------------------------------------------------------
static LCM_UTIL_FUNCS lcm_util = {0};

// ---------------------------------------------------------------------------
//  Local Functions
// ---------------------------------------------------------------------------
#define dsi_set_cmdq_V2(cmd, count, ppara, force_update) \
        lcm_util.dsi_set_cmdq_V2(cmd, count, ppara, force_update)
#define dsi_set_cmdq_V3(para_tbl,size,force_update)     \
        lcm_util.dsi_set_cmdq_V3(para_tbl,size,force_update)
#define wrtie_cmd(cmd)                                 lcm_util.dsi_write_cmd(cmd)
#define write_regs(addr, pdata, byte_nums)             lcm_util.dsi_write_regs(addr, pdata, byte_nums)
#define read_reg(cmd)                                  lcm_util.dsi_dcs_read_lcm_reg(cmd)
#define read_reg_v2(cmd, buffer, buffer_size)           lcm_util.dsi_dcs_read_lcm_reg_v2(cmd, buffer, buffer_size)

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
    memcpy(&lcm_util, util, sizeof(LCM_UTIL_FUNCS));
}

// ---------------------------------------------------------------------------
//  LCM Driver Implementations
// ---------------------------------------------------------------------------

/*
 * init sequence: sleep out -> wait for the panel's internal regulator and
 * TE detection -> display on.  No NT71397 register programming required on
 * this module revision.
 */
static void lcm_init_nt71397(void)
{
    LCD_DEBUG("[NT71397] init\n");

    msleep(20);
    dsi_set_cmdq_V3(0,0,0);

    // sleep out
    wrtie_cmd(0x11);
    LCD_DEBUG("[NT71397] send command 0x11\n");
    msleep(120);

    // display on
    wrtie_cmd(0x29);
    LCD_DEBUG("[NT71397] send command 0x29\n");
    msleep(20);
}

static void lcm_sleep_nt71397(void)
{
    LCD_DEBUG("[NT71397] sleep\n");

    // display off
    wrtie_cmd(0x28);
    LCD_DEBUG("[NT71397] send command 0x28\n");
    msleep(20);

    // sleep out
    wrtie_cmd(0x10);
    LCD_DEBUG("[NT71397] send command 0x10\n");
    msleep(120);
}

static void lcm_suspend_nt71397(void)
{
    lcm_util->set_backlight(0);
    msleep(20);
    lcm_sleep_nt71397();
}

static void lcm_resume_nt71397(void)
{
    lcm_init_nt71397();
    lcm_util->set_backlight(1);
}

static void lcm_set_backlight_nt71397(unsigned int level)
{
    LCD_DEBUG("[NT71397] backlight = %d\n", level);
    lcm_util->set_backlight(level);
}

static unsigned int lcm_read_ddc(BYTE addr)
{
    return read_reg(addr);
}

static unsigned int lcm_read_ddc_v2(BYTE addr, LCM_DDCS_CALDDCS_RESULT_T *out)
{
    BYTE readdata[3];
    DSTATUS_I2C d0, d1, d2;

    d0 = 0;
    d1 = 0;
    d2 = 0;

    read_reg_v2(addr, readdata, 3);

    d0 = readdata[0];
    d1 = readdata[1];
    d2 = readdata[2];

    out->read_data = (d0 << 16) | (d1 << 8) | d2;

    return 0;
}

static void lcm_get_lcd_scan_mode(unsigned int *scan_mode)
{
    *scan_mode = interface | ARRAY_MUX | BYTE_P | DC_SYNC_ON_LO | DE_MIRROR_ON_V;
}

static void lcm_get_params(LCM_PARAMS *params)
{
    memset(params, 0, sizeof(LCM_PARAMS));

    params->type                           = LCM_TYPE_DSI;
    params->width                          = FRAME_WIDTH;
    params->height                         = FRAME_HEIGHT;

    // The panel works in sync start/end (pulse) mode per the datasheet
    // "Non-Burst Transmission with Sync Start and End (Pulse Mode)".
    params->dsi.mode                       = SYNC_PULSE_VDO_MODE;

    // DSI: 4 lanes, RGB888, MSB first, 24bit packed short packet.
    params->dsi.LANE_NUM                   = LCM_FOUR_LANE;
    params->dsi.data_format.color_order    = LCM_COLOR_ORDER_RGB;
    params->dsi.data_format.trans_seq      = LCM_DSI_TRANS_SEQ_MSB_FIRST;
    params->dsi.data_format.padding        = LCM_DSI_PADDING_ON_LSB;
    params->dsi.data_format.format         = LCM_DSI_FORMAT_RGB888;
    params->dsi.PS                         = LCM_PACKED_PS_24BIT_RGB888;

    // Timing from the datasheet line/frame period diagrams.
    //   H: 20 + 52 + 1920 + 152 = 2144 dotclk
    //   V: 6 + 48 + 1200 + 24 = 1278 lines
    params->dsi.horizontal_sync_active     = 20;
    params->dsi.horizontal_backporch       = 52;
    params->dsi.horizontal_frontporch      = 152;
    params->dsi.horizontal_active_pixel    = FRAME_WIDTH;

    params->dsi.vertical_sync_active       = 6;
    params->dsi.vertical_backporch         = 48;
    params->dsi.vertical_frontporch        = 24;
    params->dsi.vertical_active_line       = FRAME_HEIGHT;

    // The NT71397 is a Novatek receiver: keep the compatibility workarounds
    // that the other Novatek-bridged 3.10 drivers in this tree use.
    params->dsi.PLL_CLOCK                  = 234;
    params->dsi.compatibility_for_nvk      = 1;
    params->dsi.ssc_disable                = 1;
}

static unsigned int lcm_dsi_cmd(void)
{
    return 0;
}

static unsigned int lcm_dsi_get_parameter(DBUFFER_CAST params)
{
    u16 *params_get = (u16 *)params;

    params_get[0] = 0x0770;    // resolution x
    params_get[1] = 0x04b0;    // resolution y
    params_get[2] = 0x04;      // lane number

    return params_get[2];
}

LCM_DRIVER nt71397_wuxga_dsi_vdo_lcm_drv = {
    .name                       = "nt71397_wuxga_dsi_vdo",
    .set_util_funcs             = lcm_set_util_funcs,
    .get_params                 = lcm_get_params,

    .init                       = lcm_init_nt71397,
    .suspend                    = lcm_suspend_nt71397,
    .resume                     = lcm_resume_nt71397,
    .set_backlight              = lcm_set_backlight_nt71397,

    .read_ddc                   = lcm_read_ddc,
    .read_ddc_v2                = lcm_read_ddc_v2,

    .get_lcd_scan_mode          = lcm_get_lcd_scan_mode,

    .dsi_cmd                    = lcm_dsi_cmd,
    .dsi_get_parameter          = lcm_dsi_get_parameter,

    .set_tear                   = NULL,
    .set_tear_off               = NULL,
};