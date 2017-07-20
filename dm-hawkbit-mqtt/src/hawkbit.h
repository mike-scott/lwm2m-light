/*
 * Copyright (c) 2016 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"

#if (CONFIG_DM_BACKEND == BACKEND_HAWKBIT)

#define HAWKBIT_HOST	"gitci.com:8080"
#define HAWKBIT_IPADDR	"fe80::d4e7:0:0:1"
#define HAWKBIT_PORT	8080
#define HAWKBIT_JSON_URL "/DEFAULT/controller/v1"

extern int poll_sleep;

int hawkbit_ddi_poll(void);

#endif /* (CONFIG_DM_BACKEND == BACKEND_HAWKBIT) */
