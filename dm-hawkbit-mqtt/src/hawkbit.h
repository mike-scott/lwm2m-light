/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define HAWKBIT_HOST	"gitci.com:8080"
#define HAWKBIT_IPADDR	"fe80::d4e7:0:0:1"
#define HAWKBIT_PORT	8080
#define HAWKBIT_JSON_URL "/DEFAULT/controller/v1"

extern int poll_sleep;

/* TODO: Use the proper reboot Zephyr API instead (enabled via Kconfig) */
extern void sys_arch_reboot(int type);
int hawkbit_ddi_poll(void);
