/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2020, Linaro Limited
 */

#ifndef LIBCKTEEC_LOCAL_UTILS_H
#define LIBCKTEEC_LOCAL_UTILS_H

#include <stdio.h>
#include <stdlib.h>

#define LOG_ERROR(...)	printf(__VA_ARGS__)
#define LOG_INFO(...)	printf(__VA_ARGS__)
#define LOG_DEBUG(...)	printf(__VA_ARGS__)

#define ARRAY_SIZE(array)	(sizeof(array) / sizeof(array[0]))

#endif /*LIBCKTEEC_LOCAL_UTILS_H*/
