// SPDX-License-Identifier: GPL-2.0
//
// kundervolt: a kernel module to undervolt Intel-based Linux system with Secure Boot enabled.
// Copyright © 2025  Alessandro Balducci
//
// The full licence notice is available in the included README.md

#include "common.h"
#include "fp_util.h"
#include "ftoa/ftoa.h"
#include "test.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <asm/fpu/api.h>

#define VOLTAGE_HIGH_BITS_MASK 0xFFE00000

/*
 * Past this many decimal places the divisor below would overflow an int and
 * silently yield a completely wrong offset. The hardware field resolution is
 * ~0.98 mV, so six places is already far more than can ever be represented.
 */
#define MAX_DECIMAL_PLACES 6

// Not the ideal way to round but it's easy
#define ROUND(x) ((int)(((x) < 0) ? ((x) - 0.5) : ((x) + 0.5)))

static int ctod(char c) {
	if (c < '0' || c > '9') {
		return UERR;
	}
	return c - '0';
}

/**
 * Converts ascii string into float.
 *
 * Not a great implementation but it works well enough under our constraints.
 *
 * Must always be called inside `kernel_fpu_begin/end()` guards.
*/
static int atof(const char* s, size_t s_size, float* out) {
	int integer = 0;
	float decimal = 0;
	size_t i = 0;
	char c;
	bool integer_finished = false;
	bool has_digit = false;
	int sign = 1;
	size_t cur_decimal_place = 0;

	while (i < s_size && (c = s[i])) {
		/*
		 * Accept a trailing newline so that a plain `echo` works and not only
		 * `echo -n`. Fighting spurious "invalid input" errors is exactly the
		 * situation in which a user mistypes a voltage.
		 */
		if (c == '\n' || c == '\r') {
			break;
		}

		if (i == 0 && c == '-') {
			sign = -1;
			i++;
			continue;
		}

		if (!integer_finished) {
			if (c == '.') {
				integer_finished = true;
			} else {
				int d = ctod(c);
				if (d == UERR) {
					// Invalid character
					pr_err(LOGHDR "Invalid character (%d)\n", c);
					return UERR;
				}
				has_digit = true;

				int tmp;
				if (check_mul_overflow(integer, 10, &tmp)) {
					pr_err(LOGHDR "Overflow mul\n");
					return UERR;
				}
				if (check_add_overflow(tmp, d, &integer)) {
					pr_err(LOGHDR "Overflow add\n");
					return UERR;
				}
			}
		} else {
			int d = ctod(c);
			if (d == UERR) {
				// Invalid character
				pr_err(LOGHDR "Invalid character (%d)\n", c);
				return UERR;
			}
			has_digit = true;

			// Extra digits are still validated, just not accumulated
			if (cur_decimal_place < MAX_DECIMAL_PLACES) {
				int divisor = 10;
				for (size_t j = 0; j < cur_decimal_place; j++) {
					divisor *= 10;
				}

				decimal += (float)d / divisor;
			}
			cur_decimal_place++;
		}
		i++;
	}

	// Reject "", "-" and "." rather than silently treating them as zero
	if (!has_digit) {
		pr_err(LOGHDR "No digits in input\n");
		return UERR;
	}

	*out = integer;
	*out += decimal;
	*out *= sign;

	return 0;
}

/**
 * Offset calculation obtained from https://github.com/mihic/linux-intel-undervolt
 *
 * Arguments:
 *	mv_offset, voltage offset in millivolts
 *
 * Returns the voltage as an integer value compatible with the MSR 0x150 format
 *
 * Steps:
 * 1. Multiply by 1.024
 * 2. Round to nearest integer
 * 3. Shift left by 21
 * 4. Only retain the high 11 bits
 *
 * Must always be called inside `kernel_fpu_begin/end()` guards.
*/
static intoff_t offset_mv_to_int(float mv_offset) {
	int rounded_product = ROUND(mv_offset * 1.024);

	// Shift as unsigned: left shifting a negative value is undefined behaviour
	return VOLTAGE_HIGH_BITS_MASK & ((uint32_t)rounded_product << 21);
}

/**
 * Inverse of `offset_mv_to_int()`
 *
 * Must always be called inside `kernel_fpu_begin/end()` guards.
*/
static float offset_int_to_mv(intoff_t offset) {
	return (offset >> 21) / 1.024;
}

size_t offset_int_to_mv_str(char* buf, size_t buf_size, intoff_t offset) {
	kernel_fpu_begin();
	float mv = offset_int_to_mv(offset);
	size_t written = ftoa(buf, buf_size, mv, 2);
	kernel_fpu_end();
	return written;
}

int offset_mv_str_to_int(intoff_t* offset, const char* buf, size_t buf_size) {
	float mv;
	int status = 0;
	intoff_t encoded = 0;

	/*
	 * Every floating point operation must happen inside this one guard,
	 * including the range comparisons below and the float argument passed to
	 * offset_mv_to_int(). This file is compiled with SSE enabled, so an
	 * unguarded float compare emits XMM instructions that would corrupt the
	 * FPU register state of whichever userspace task is writing to sysfs.
	 */
	kernel_fpu_begin();
	if (atof(buf, buf_size, &mv)) {
		status = UERR;
	} else if (mv < VOLTAGE_RANGE_MIN || mv > VOLTAGE_RANGE_MAX) {
		status = UERR_RANGE;
#ifdef LOCK_OVERVOLT
	} else if (mv > 0) {
		status = UERR_OVERVOLT;
#endif
	} else {
		encoded = offset_mv_to_int(mv);
	}
	kernel_fpu_end();

	if (status) {
		return status;
	}

	*offset = encoded;
	return 0;
}

#ifdef TESTS

// Test helper: wraps the now-private, guard-requiring conversion
static intoff_t test_mv_to_int(float mv) {
	intoff_t res;
	kernel_fpu_begin();
	res = offset_mv_to_int(mv);
	kernel_fpu_end();
	return res;
}

static int offset_mv_to_int_test1(void) {
	ASSERT_EQ_HEX(test_mv_to_int(-50), 0xF9A00000);
	return 0;
}

static int offset_mv_to_int_test2(void) {
	ASSERT_EQ_HEX(test_mv_to_int(-150.4), 0xECC00000);
	return 0;
}

static int offset_mv_to_int_test3(void) {
	ASSERT_EQ_HEX(test_mv_to_int(-125.0), 0xF0000000);
	return 0;
}

static int offset_mv_to_int_test4(void) {
	ASSERT_EQ_HEX(test_mv_to_int(-4), 0xFF800000);
	return 0;
}

static int offset_int_to_mv_test(void) {
	for (int i = -999; i < 1000; i++) {
		// Single guard for the whole round trip, no nesting
		kernel_fpu_begin();
		int offset = offset_mv_to_int(i);
		float offset_mv = offset_int_to_mv(offset);
		int reconverted_offset = offset_mv_to_int(offset_mv);
		kernel_fpu_end();
		ASSERT_EQ_HEX(offset, reconverted_offset);
	}
	return 0;
}

static int atof_test1(void) {
	kernel_fpu_begin();
	float f;
	int res = atof("0.0", 4, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, 0);
	ASSERT_EQ(f, 0.0);
	return 0;
}

static int atof_test2(void) {
	kernel_fpu_begin();
	float f;
	int res = atof(".5", 3, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, 0);
	ASSERT_EQ(f, 0.5);
	return 0;
}

static int atof_test3(void) {
	kernel_fpu_begin();
	float f;
	int res = atof("-50.25", 7, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, 0);
	ASSERT_EQ(f, -50.25);
	return 0;
}

static int atof_test4(void) {
	kernel_fpu_begin();
	float f;
	int res = atof("196.75", 7, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, 0);
	ASSERT_EQ(f, 196.75);
	return 0;
}

static int atof_test5(void) {
	kernel_fpu_begin();
	float f;
	int res = atof("-999", 5, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, 0);
	ASSERT_EQ(f, -999);
	return 0;
}

// A trailing newline is accepted, so plain `echo` works
static int atof_test_newline(void) {
	kernel_fpu_begin();
	float f;
	int res = atof("-50\n", 5, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, 0);
	ASSERT_EQ(f, -50);
	return 0;
}

// Digits past MAX_DECIMAL_PLACES are ignored instead of overflowing the divisor
static int atof_test_long_decimal(void) {
	kernel_fpu_begin();
	float f;
	int res = atof("-0.500000000000000000000001", 27, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, 0);
	ASSERT_EQ(f, -0.5);
	return 0;
}

static int atof_test_error1(void) {
	kernel_fpu_begin();
	float f;
	int res = atof("1.0.4", 6, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, -1);
	return 0;
}

static int atof_test_error2(void) {
	kernel_fpu_begin();
	float f;
	int res = atof("11.55asd", 9, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, -1);
	return 0;
}

static int atof_test_error3(void) {
	kernel_fpu_begin();
	float f;
	int res = atof("--1", 4, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, -1);
	return 0;
}

// Empty input is an error, not zero
static int atof_test_error4(void) {
	kernel_fpu_begin();
	float f;
	int res = atof("", 1, &f);
	kernel_fpu_end();
	ASSERT_EQ_INT(res, -1);
	return 0;
}

static int offset_mv_str_to_int_test1(void) {
	intoff_t offset;
	int ret = offset_mv_str_to_int(&offset, "-50", 4);
	ASSERT_EQ_INT(ret, 0);
	ASSERT_EQ_HEX(offset, 0xF9A00000);
	return 0;
}
static int offset_mv_str_to_int_test2(void) {
	intoff_t offset;
	int ret = offset_mv_str_to_int(&offset, "-150.4", 7);
	ASSERT_EQ_INT(ret, 0);
	ASSERT_EQ_HEX(offset, 0xECC00000);
	return 0;
}

static int offset_mv_str_to_int_test3(void) {
	intoff_t offset;
	int ret = offset_mv_str_to_int(&offset, "-125.0", 7);
	ASSERT_EQ_INT(ret, 0);
	ASSERT_EQ_HEX(offset, 0xF0000000);
	return 0;
}

static int offset_mv_str_to_int_test4(void) {
	intoff_t offset;
	int ret = offset_mv_str_to_int(&offset, "-4", 3);
	ASSERT_EQ_INT(ret, 0);
	ASSERT_EQ_HEX(offset, 0xFF800000);
	return 0;
}

// Out of range input must not touch the caller's offset
static int offset_mv_str_to_int_test_range(void) {
	intoff_t offset = 0x5A5A5A5A;
	int ret = offset_mv_str_to_int(&offset, "-1000", 6);
	ASSERT_EQ_INT(ret, UERR_RANGE);
	ASSERT_EQ_HEX(offset, 0x5A5A5A5A);
	return 0;
}

int run_fp_tests(void) {
	INIT_TEST_SUITE("Floating Point utilities")

	RUN_TEST(offset_mv_to_int_test1)
	RUN_TEST(offset_mv_to_int_test2)
	RUN_TEST(offset_mv_to_int_test3)
	RUN_TEST(offset_mv_to_int_test4)
	RUN_TEST(offset_int_to_mv_test)
	RUN_TEST(atof_test1)
	RUN_TEST(atof_test2)
	RUN_TEST(atof_test3)
	RUN_TEST(atof_test4)
	RUN_TEST(atof_test5)
	RUN_TEST(atof_test_newline)
	RUN_TEST(atof_test_long_decimal)
	RUN_TEST(atof_test_error1)
	RUN_TEST(atof_test_error2)
	RUN_TEST(atof_test_error3)
	RUN_TEST(atof_test_error4)
	RUN_TEST(offset_mv_str_to_int_test1)
	RUN_TEST(offset_mv_str_to_int_test2)
	RUN_TEST(offset_mv_str_to_int_test3)
	RUN_TEST(offset_mv_str_to_int_test4)
	RUN_TEST(offset_mv_str_to_int_test_range)

	END_TEST_SUITE
}

#endif // TESTS
