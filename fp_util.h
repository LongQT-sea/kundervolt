/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kundervolt: a kernel module to undervolt Intel-based Linux system with Secure Boot enabled.
 * Copyright © 2025  Alessandro Balducci
 *
 * The full licence notice is available in the included README.md
*/

#pragma once

#include <linux/kernel.h>
#include "test.h"

typedef int32_t intoff_t;

#define VOLTAGE_RANGE_MIN -999
#define VOLTAGE_RANGE_MAX 999

/*
 * Only these two functions are exported.
 *
 * Everything that takes or returns a `float` is deliberately kept private to
 * fp_util.c: this translation unit is built with SSE enabled (see the Makefile)
 * and passing a float across a function boundary is itself an FPU operation, so
 * such a function could never be called safely from outside a
 * kernel_fpu_begin/end() region. Both functions below open that region
 * internally and no float ever escapes it.
 */
size_t offset_int_to_mv_str(char* buf, size_t buf_size, intoff_t offset);
int offset_mv_str_to_int(intoff_t* offset, const char* buf, size_t buf_size);

#ifdef TESTS
int run_fp_tests(void);
#endif // TESTS
