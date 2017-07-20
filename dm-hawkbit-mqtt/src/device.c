/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr.h>
#include <soc.h>
#include <gpio.h>
#include "device.h"

/*
 * General hardware specific configs
 *
 * DEVICE_ID_BASE  : beginning of HW UID registers
 * DEVICE_ID_LENGTH: length of HW UID registers in 32-bit words
 */
#if defined(CONFIG_SOC_SERIES_NRF52X)
#define DEVICE_ID_BASE		(&NRF_FICR->DEVICEID[0])
#define DEVICE_ID_LENGTH	2
#elif defined(CONFIG_SOC_SERIES_STM32F4X)
#define DEVICE_ID_BASE		UID_BASE
#define DEVICE_ID_LENGTH	3
#elif defined(CONFIG_SOC_SERIES_KINETIS_K6X)
#define DEVICE_ID_BASE		(&SIM->UIDH)
#define DEVICE_ID_LENGTH	4
#endif


struct product_id_t product_id = {
	.name = CONFIG_BOARD,
};

#define HASH_MULTIPLIER		37
static uint32_t hash32(char *str, int len)
{
	uint32_t h = 0;
	int i;

	for (i = 0; i < len; ++i) {
		h = (h * HASH_MULTIPLIER) + str[i];
	}

	return h;
}

/* Find and set common unique device specific information */
void set_device_id(void)
{
	int i;
	char buffer[DEVICE_ID_LENGTH*8 + 1];

	for (i = 0; i < DEVICE_ID_LENGTH; i++) {
		sprintf(buffer + i*8, "%08x",
			*(((uint32_t *)DEVICE_ID_BASE) + i));
	}

	product_id.number = hash32(buffer, DEVICE_ID_LENGTH*8);
}
