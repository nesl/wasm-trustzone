/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"

uint64
os_time_get_boot_microsecond()
{
    return k_uptime_get_32() * 1000;
}

uint32
os_time_get_renju_boot_microsecond()
{
  return k_uptime_get_32_us();
}

uint32
get_renju_rand()
{
  return sys_rand32_get();
}

void sleep_us(uint32 microseconds)
{
  k_usleep(microseconds);
}
