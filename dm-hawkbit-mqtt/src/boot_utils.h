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

/* Required by the bootloader in order to trigger the update */
#define BOOT_IMG_MAGIC		0x12344321

/* Defined by mynewt's bootloader */
struct boot_img_trailer {
	uint32_t bit_copy_start;
	uint8_t  bit_copy_done;
	uint8_t  bit_img_ok;
	uint16_t _pad;
};

typedef enum {
	BOOT_ACID_CURRENT = 0,
	BOOT_ACID_UPDATE,
} boot_acid_t;

struct boot_acid {
	uint32_t current;
	uint32_t update;
};

uint8_t boot_status_read(void);
void boot_status_update(void);
void boot_trigger_ota(void);
uint32_t boot_acid_read(boot_acid_t type);
void boot_acid_update(boot_acid_t type, uint32_t acid);
