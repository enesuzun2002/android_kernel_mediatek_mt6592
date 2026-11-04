/*
 * LCM driver for the ETAB5 10.1" WUXGA panel (OSD100T2587-53TS module with
 * a Novatek NT71397 DSI-to-LVDS/RGB bridge).
 *
 * Every parameter and the power/init sequence below were recovered from the
 * stock ETAB5 3.10.72 kernel (device/generalmobile/hulkbuster/kernel), whose
 * LCM driver has the same name. Its LCM_DRIVER provides only set_util_funcs,
 * get_params, init, suspend and resume.
 *
 * Notable differences from a plain DCS panel:
 *   - no DCS sleep-out/display-on: the bridge is started with a single
 *     "Turn On Peripheral" (data type 0x32) packet
 *   - GPIO119 powers/enables the panel path (dropped on suspend) and GPIO118
 *     is raised once at init
 *   - burst video mode with a 441 MHz DSI PLL; 1920x1200x24bpp needs
 *     ~800 Mbit/s per lane on 4 lanes, so a slower link starves the stream
 *     (no frames, "Wait VSync timeout")
 *
 * Selected at boot from "lcm=0-nt71397_wuxga_dsi_vdo" on the command line.
 */

#ifndef BUILD_LK
#include <linux/string.h>
#endif
#include "lcm_drv.h"

#ifdef BUILD_LK
    #include <platform/mt_gpio.h>
#else
    #include <mach/mt_gpio.h>
#endif

#define FRAME_WIDTH		(1920)
#define FRAME_HEIGHT		(1200)

#define GPIO_LCM_PWR_EN		(GPIO119 | 0x80000000)
#define GPIO_LCM_EN2		(GPIO118 | 0x80000000)

static LCM_UTIL_FUNCS lcm_util = {0};

#define SET_RESET_PIN(v)		(lcm_util.set_reset_pin((v)))
#define MDELAY(n)			(lcm_util.mdelay(n))
#define dsi_set_cmdq(pdata, queue_size, force_update) \
	lcm_util.dsi_set_cmdq(pdata, queue_size, force_update)
#define set_gpio_mode(pin, mode)	(lcm_util.set_gpio_mode((pin), (mode)))
#define set_gpio_dir(pin, dir)		(lcm_util.set_gpio_dir((pin), (dir)))
#define set_gpio_out(pin, out)		(lcm_util.set_gpio_out((pin), (out)))

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(LCM_UTIL_FUNCS));
}

static void lcm_get_params(LCM_PARAMS *params)
{
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type   = LCM_TYPE_DSI;
	params->width  = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;

	params->dsi.mode     = BURST_VDO_MODE;
	params->dsi.LANE_NUM = LCM_FOUR_LANE;

	params->dsi.data_format.color_order = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq   = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding     = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format      = LCM_DSI_FORMAT_RGB888;

	params->dsi.packet_size = 256;
	params->dsi.intermediat_buffer_num = 0;
	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.word_count = FRAME_WIDTH * 3;

	params->dsi.vertical_sync_active   = 2;
	params->dsi.vertical_backporch     = 18;
	params->dsi.vertical_frontporch    = 15;
	params->dsi.vertical_active_line   = FRAME_HEIGHT;

	params->dsi.horizontal_sync_active  = 16;
	params->dsi.horizontal_backporch    = 32;
	params->dsi.horizontal_frontporch   = 16;
	params->dsi.horizontal_active_pixel = FRAME_WIDTH;

	params->dsi.PLL_CLOCK = 476;
}

static void lcm_reset(void)
{
	SET_RESET_PIN(1);
	MDELAY(10);
	SET_RESET_PIN(0);
	MDELAY(10);
	SET_RESET_PIN(1);
	MDELAY(10);
}

static void lcm_turn_on_peripheral(void)
{
	unsigned int data_array[16];

	data_array[0] = 0x00053200;
	dsi_set_cmdq(data_array, 1, 1);
}

static void lcm_init(void)
{
	lcm_reset();

	set_gpio_mode(GPIO_LCM_PWR_EN, GPIO_MODE_00);
	set_gpio_dir(GPIO_LCM_PWR_EN, GPIO_DIR_OUT);
	set_gpio_out(GPIO_LCM_PWR_EN, GPIO_OUT_ONE);
	MDELAY(150);

	lcm_turn_on_peripheral();

	set_gpio_mode(GPIO_LCM_EN2, GPIO_MODE_00);
	set_gpio_dir(GPIO_LCM_EN2, GPIO_DIR_OUT);
	set_gpio_out(GPIO_LCM_EN2, GPIO_OUT_ONE);
}

static void lcm_suspend(void)
{
	MDELAY(90);
	set_gpio_out(GPIO_LCM_PWR_EN, GPIO_OUT_ZERO);
}

static void lcm_resume(void)
{
	lcm_reset();

	set_gpio_mode(GPIO_LCM_PWR_EN, GPIO_MODE_00);
	set_gpio_dir(GPIO_LCM_PWR_EN, GPIO_DIR_OUT);
	set_gpio_out(GPIO_LCM_PWR_EN, GPIO_OUT_ONE);
	MDELAY(150);
	MDELAY(50);

	lcm_turn_on_peripheral();
	/*
	 * GPIO118 is raised once in lcm_init() and never driven low (neither
	 * here nor by stock), so it does not need re-raising on resume; doing
	 * so after a 200 ms wait only delayed every screen wake.
	 */
}

LCM_DRIVER nt71397_wuxga_dsi_vdo_lcm_drv = {
	.name           = "nt71397_wuxga_dsi_vdo",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params     = lcm_get_params,
	.init           = lcm_init,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
};
