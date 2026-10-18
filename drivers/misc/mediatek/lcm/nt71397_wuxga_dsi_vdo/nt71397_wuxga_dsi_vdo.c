/*
 * LCM driver for the OSD Displays OSD100T2587-53TS 10.1" WUXGA module.
 *
 * Module characteristics (datasheet OSD100T2587-53TS rev A.1, 2016-07-11):
 *   - 1920 x 1200 IPS, normally black, RGB888
 *   - MIPI DSI, video mode, 4 data lanes, no RAM on the module
 *   - Non-burst transmission with sync start/end (pulse mode)
 *   - Horizontal: HSA 20 + HBP 52 + 1920 + HFP 152 = 2144 dotclk
 *     Vertical:   VSA 6  + VBP 48 + 1200 + VFP 24  = 1278 lines
 *     (dotclk 164.4 MHz @ 60 Hz typical; the ETAB5 loader asks for 55.71 Hz
 *      via the "fps=" kernel command line parameter)
 *   - Touch: FocalTech FT5526 on the same flex (i2c 0x38, handled by ft5x0x)
 *
 * The DSI receiver bridge on the module is a Novatek NT71397: a fixed
 * function DSI-to-RGB converter. It re-drives the panel from the video
 * stream, so no RAM/command-stream driver is required - just the standard
 * DSI sleep-out/display-on packets. The module has no reset or enable pin
 * (pin section 5.1: only T_GND / VCC / MIPI pairs / LED cathodes), so no
 * board GPIO handling is needed. Backlight runs through the MTK BLS PWM
 * path.
 *
 * Selected automatically at boot: the bootloader passes
 *   lcm=0-nt71397_wuxga_dsi_vdo
 * in the kernel command line, and mtkfb strips the "0-" prefix and matches
 * the remainder against this driver's name.
 */

#ifndef BUILD_LK
#include <linux/string.h>
#endif
#include "lcm_drv.h"

#ifdef BUILD_LK
    #include <platform/disp_drv_platform.h>
#else
    #include <linux/delay.h>
    #include <mach/mt_gpio.h>
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

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
    memcpy(&lcm_util, util, sizeof(LCM_UTIL_FUNCS));
}

// ---------------------------------------------------------------------------
//  LCM Driver Implementations
// ---------------------------------------------------------------------------

static void lcm_power_init(int enable)
{
    // Nothing to do: the panel module has no enable/reset line.
}

static void lcm_init(void)
{
    LCD_DEBUG("[NT71397] init\n");

    msleep(20);

    // sleep out
    dsi_set_cmdq_V2(0x11, 0, NULL, 1);
    msleep(120);

    // display on
    dsi_set_cmdq_V2(0x29, 0, NULL, 1);
    msleep(20);
}

static void lcm_suspend(void)
{
    LCD_DEBUG("[NT71397] suspend\n");

    // display off
    dsi_set_cmdq_V2(0x28, 0, NULL, 1);
    msleep(20);

    // sleep in
    dsi_set_cmdq_V2(0x10, 0, NULL, 1);
    msleep(120);
}

static void lcm_resume(void)
{
    LCD_DEBUG("[NT71397] resume\n");
    lcm_init();
}

static unsigned int lcm_compare_id(void)
{
    // This module has no readable ID through the video-mode bridge.
    return 1;
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

    // The NT71397 is a Novatek receiver: keep the same workarounds the other
    // Novatek-bridged drivers in this tree use.
    params->dsi.PLL_CLOCK                  = 234;
    params->dsi.compatibility_for_nvk      = 1;
    params->dsi.ssc_disable                = 1;
}

LCM_DRIVER nt71397_wuxga_dsi_vdo_lcm_drv = {
    .name           = "nt71397_wuxga_dsi_vdo",
    .set_util_funcs = lcm_set_util_funcs,
    .get_params     = lcm_get_params,
    .init_power     = lcm_power_init,
    .init           = lcm_init,
    .suspend        = lcm_suspend,
    .resume         = lcm_resume,
    .compare_id     = lcm_compare_id,
};