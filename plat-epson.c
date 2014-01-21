/*
  Plastic Logic EPD project on MSP430

  Copyright (C) 2013 Plastic Logic Limited

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
 * plat-hbz6.c -- Plastic Logic Hummingbird Z6 adapter
 *
 * Authors: Andrew Cox <andrew.cox@plasticlogic.com>
 *          Guillaume Tucker <guillaume.tucker@plasticlogic.com>
 *
 */

#include "platform.h"
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "assert.h"
#include "msp430-gpio.h"
#include "i2c.h"
#include "vcom.h"
#include "msp430-i2c.h"
#include "FatFs/ff.h"
#include "pmic-tps65185.h"
#include "pmic-max17135.h"
#include "epson/S1D13541.h"
#include "epson/S1D13524.h"
#include "epson/epson-i2c.h"
#include "epson/epson-utils.h"
#include "epson/epson-if.h"
#include "plat-hbz6.h"
#include "plat-raven.h"
#include "temp-lm75.h"
#include "slideshow.h"
#include "i2c-eeprom.h"
#include "psu-data.h"
#include "config.h"

#define LOG_TAG "platform"
#include "utils.h"

#define CONFIG_PLAT_RUDDOCK2	1
#if CONFIG_PLAT_RUDDOCK2
#define B_HWSW_CTRL	GPIO(1,2)
#define B_POK		GPIO(1,0)
#define B_PMIC_EN	GPIO(1,1)
#define EPSON_CS_0	GPIO(3,6)
#define EPSON_CLK_EN 	GPIO(1,6)
#define EPSON_3V3_EN 	GPIO(1,7)
#endif

/* 1 to cycle power supplies only, no display update (testing only) */
#define	CONFIG_PSU_ONLY 0

/* I2C addresses */
#define I2C_MAX17135_PMIC_ADDR  0x48
#define I2C_TPS65185_PMIC_ADDR 0x68
#define	I2C_TEMP_SENSOR		   0x49
#define I2C_PSU_EEPROM_ADDR    0x50
#define I2C_PLWF_EEPROM_ADDR   0x54

static struct tps65185_info *tps65185_pmic_info;
static struct max17135_info *max17135_pmic_info;
static struct i2c_adapter *i2c;
static struct s1d135xx *epson;
static struct vcom_cal vcom_calibration;
static struct lm75_info *lm75_info;
#if !CONFIG_WF_ON_SD_CARD
static struct i2c_eeprom *plwf_eeprom;
static struct plwf_data plwf_data;
#endif

static struct psu_data psu_data;

/* Fallback VCOM calibration data if PSU EEPROM corrupt */
static const struct vcom_info DEF_VCOM_PSU = {
	.dac_x1 = 127,
	.dac_y1 = 4172,
	.dac_x2 = 381,
	.dac_y2 = 12490,
	.vgpos_mv = 25080,
	.vgneg_mv = -32300,
};

#define VCOM_VGSWING 56886

int check_platform()
{
#if CONFIG_USE_PSU_EEPROM
	struct i2c_adapter *i2c;
	struct i2c_eeprom *psu_eeprom;

	msp430_i2c_init(0, &i2c);
	eeprom_init(i2c, 0x50, EEPROM_24LC014, &psu_eeprom);

	if (psu_data_init(&psu_data, psu_eeprom)) {
		abort_msg("Failed to initialise VCOM PSU data from EEPROM");
	}

	return psu_data.hw_info.epdc_ref;
#endif
	return EPDC_NONE;
}

int plat_epson_init()
{
	int ret = 0;
	screen_t prev_screen;
	int vcom;
	char * platform_path = "0:/";
	/* Filesystem Check */

#if CONFIG_WF_ON_SD_CARD
	check(f_chdir(CONFIG_DISPLAY_TYPE) == FR_OK);
	/* read the display vcom */
	vcom = util_read_vcom();
	assert(vcom > 0);
#endif

	/* initialise the Epson interface */
	epsonif_init(0, 1);

	/* Set the waveform id table for the current controller */
	s1d135xx_set_wfid_table(psu_data.hw_info.epdc_ref);

	/* define GPIOs required for operation */
	ret |= gpio_request(EPSON_CS_0,	PIN_GPIO | PIN_OUTPUT | PIN_INIT_HIGH);

	if (psu_data.hw_info.epdc_ref == EPDC_S1D13541) {
		ret |= gpio_request(B_HWSW_CTRL,PIN_GPIO | PIN_OUTPUT | PIN_INIT_LOW);
		ret |= gpio_request(B_PMIC_EN,	PIN_GPIO | PIN_OUTPUT | PIN_INIT_LOW);
		ret |= gpio_request(B_POK, 	  	PIN_GPIO | PIN_INPUT);
		ret |= gpio_request(EPSON_3V3_EN, PIN_GPIO | PIN_OUTPUT | PIN_INIT_HIGH);
		ret |= gpio_request(EPSON_CLK_EN, PIN_GPIO | PIN_OUTPUT | PIN_INIT_HIGH);
	}

	if (ret)
		return -1;

	/* initialise the Epson controller */
	switch (psu_data.hw_info.epdc_ref) {
	case EPDC_NONE:
		break;
	case EPDC_S1D13524:
		/* TODO: Proper display eeprom support for Raven */
		check(f_chdir("0:/Type-11") == FR_OK);
		check(s1d13524_init(EPSON_CS_0, &epson)==0);
		break;
	case EPDC_S1D13541:
#if !CONFIG_PSU_ONLY
		/* Epson S1D13541 controller early init */
		check(s1d13541_init_start(EPSON_CS_0, &prev_screen, &epson) == 0);
		check(s1d13541_init_prodcode(epson) == 0);
		check(s1d13541_init_clock(epson) == 0);
#endif
		break;
	default:
		break;
	}

	switch(psu_data.hw_info.i2c_mode) {
	case 1:
		check(msp430_i2c_init(0, &i2c) == 0);
		break;
	case 2:
		check(epson_i2c_init(epson, &i2c) == 0);
		break;
	case 3:
		check(epson_i2c_init(epson, &i2c) == 0);
		break;
	case 4:
	case 0:
	default:
		break;
	}

	if (psu_data.hw_info.epdc_ref == EPDC_S1D13541) {
#if !CONFIG_PSU_ONLY
#if CONFIG_WF_ON_SD_CARD
		LOG("Loading display data from SD card");

		if (s1d13541_send_waveform())
			abort_msg("Failed to load waveform from SD card");
#else /* !WAVEFORM_ON_SD_CARD */
		/* TODO: Display EEPROM support for Raven */
		LOG("Loading display data from EEPROM");

		eeprom_init(i2c, I2C_PLWF_EEPROM_ADDR, EEPROM_24AA256, &plwf_eeprom);

		if (plwf_data_init(&plwf_data, plwf_eeprom))
			abort_msg("Failed to initialise display data");

		if (plwf_load_wf(&plwf_data, plwf_eeprom, epson, S1D13541_WF_ADDR))
			abort_msg("Failed to load waveform from EEPROM");

		vcom = plwf_data.info.vcom;

		strcat(platform_path, plwf_data.info.panel_type);
		printk("Checking %s exists\n", platform_path);
		check(f_chdir(platform_path) == FR_OK);

#endif /* !WAVEFORM_ON_SD_CARD */
		check(s1d13541_init_initcode(epson) == 0);
		check(s1d13541_init_pwrstate(epson) == 0);
		check(s1d13541_init_keycode(epson) == 0);
		check(s1d13541_init_gateclr(epson) == 0);
		check(s1d13541_init_end(epson, prev_screen) == 0);
#endif
	}

	/* read the psu calibration data and ready it for use */
	if (&psu_data == NULL) {
#if 0
		LOG("Using hard-coded default VCOM PSU values");
		psu_data.version = PSU_DATA_VERSION;
		memcpy(&psu_data.vcom_info, &DEF_VCOM_PSU, sizeof psu_data.vcom_info);
#else
		abort_msg("Failed to initialise VCOM PSU data from EEPROM");
#endif
	}

	/* initialise the VCOM */
	vcom_init(&vcom_calibration, &psu_data.vcom_info, psu_data.hw_info.swing_ideal);

	/* select the controller for future operations */
	s1d135xx_select(epson, &prev_screen);

	if (psu_data.hw_info.epdc_ref == EPDC_S1D13524) {
		s1d13524_set_temperature_mode(epson, TEMP_MODE_MANUAL);
	}
	else if (psu_data.hw_info.epdc_ref == EPDC_S1D13541) {
		s1d13541_set_temperature_mode(epson, TEMP_MODE_INTERNAL);
	}
	else {
		LOG("No EPDC specified...");
		ret = -1;
	}

	switch (psu_data.hw_info.hv_pmic) {
	case 1: /* Maxim MAX17135 */
		max17135_init(i2c, I2C_MAX17135_PMIC_ADDR, &max17135_pmic_info);
		max17135_configure(max17135_pmic_info, &vcom_calibration, MAX17135_SEQ_1);
		max17135_set_vcom_voltage(max17135_pmic_info, vcom);
		break;
	case 2: /* TI TPS65185 */
		tps65185_init(i2c, I2C_TPS65185_PMIC_ADDR, &tps65185_pmic_info);
		tps65185_configure(tps65185_pmic_info, &vcom_calibration);
		tps65185_set_vcom_voltage(tps65185_pmic_info, vcom);
		break;
	case 0: /* No PMIC */
	default:
		break;
	}

	/* initialise the i2c temperature sensor */
	switch (psu_data.hw_info.temp_sensor) {
	case 1: /* LM75 */
		lm75_init(i2c, I2C_TEMP_SENSOR, &lm75_info);
		break;
	case 0:
	default:
		break;
	}

	if (psu_data.hw_info.epdc_ref == EPDC_S1D13524) {
		plat_s1d13524_init_display(epson);
		plat_s1d13524_slideshow(epson);
	}
	else if (psu_data.hw_info.epdc_ref == EPDC_S1D13541) {
		plat_s1d13541_init_display(epson);
		plat_s1d13541_slideshow(epson);
	}
	else
	{
		LOG("No EPDC specified...");
		ret = -1;
	}

	s1d135xx_deselect(epson, prev_screen);

	return ret;
}
