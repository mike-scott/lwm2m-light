/*
 * Copyright (c) 2016 Linaro Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <zephyr.h>
#include <soc.h>
#include "device.h"

struct product_id_t product_id = {
	.name = CONFIG_BOARD,
};

/* Find and set common unique device specific information */
void set_device_id(void)
{
#ifdef CONFIG_SOC_FAMILY_NRF5
	product_id.number = (uint16_t) NRF_FICR->DEVICEID[0];
#elif CONFIG_SOC_SERIES_STM32F4X
	product_id.number = *(uint8_t *) DEVICE_ID_BASE +
				((*(uint8_t *) DEVICE_ID_BASE + 2) << 8);
#endif
}
