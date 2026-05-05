// SPDX-License-Identifier: GPL-2.0
//
// kundervolt: a kernel module to undervolt Intel-based Linux system with Secure Boot enabled.
// Copyright © 2025  Alessandro Balducci
//
// The full licence notice is available in the included README.md

#include "common.h"
#include "test.h"
#include "fp_util.h"
#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alessandro Balducci");
MODULE_DESCRIPTION("Experimental undervolt kernel module under SecureBoot");
MODULE_VERSION("0.1");

enum plane_index : uint64_t {
	PLANE_INDEX_CPU = 0,
	PLANE_INDEX_GPU = 1ull << 40,
	PLANE_INDEX_CACHE = 2ull << 40,
	// Also called uncore
	PLANE_INDEX_SYSTEM_AGENT = 3ull << 40,
	PLANE_INDEX_ANALOG_IO = 4ull << 40,
	// Reports say this does not work?
	// PLANE_INDEX_DIGITAL_IO = 5ull << 40,
	PLANE_INDEX_UNKNOWN = 0xFFFFFFFFFFFFFFFF
};

enum msr_operation : uint64_t {
	MSR_OP_READ = 0,
	MSR_OP_WRITE = 1ull << 32,
};

#define MSR_ADDR_VOLTAGE 0x150
#define MSR_VOLTAGE_BASE_VALUE 0x8000001000000000
#define MSR_VOLTAGE_OFFSET_MASK ((1ull << 32) - 1)

static inline uint64_t build_msr_request(enum plane_index idx,
										 enum msr_operation op,
										 intoff_t offset) {
	uint64_t extended_offset = offset;
	return MSR_VOLTAGE_BASE_VALUE | idx | op |
		   (extended_offset & MSR_VOLTAGE_OFFSET_MASK);
}

static intoff_t read_voltage_offset(enum plane_index idx) {
	uint64_t read_request = build_msr_request(idx, MSR_OP_READ, 0);
	int res = wrmsrq_safe(MSR_ADDR_VOLTAGE, read_request);
	if (res) {
		pr_err(LOGHDR "Failed to write read request to msr\n");
		return 0;
	}
	uint64_t offset = 0;
	res = rdmsrq_safe(MSR_ADDR_VOLTAGE, &offset);
	if (res) {
		pr_err(LOGHDR "Failed to read msr\n");
	}

	return offset;
}

static void write_voltage_offset(enum plane_index idx, intoff_t offset) {
#ifdef LOCK_OVERVOLT
	if (offset > 0) {
		return;
	}
#endif

	uint64_t write_request = build_msr_request(idx, MSR_OP_WRITE, offset);
	int res = wrmsrq_safe(MSR_ADDR_VOLTAGE, write_request);
	if (res) {
		pr_err(LOGHDR "Failed to write write request to msr: %llx\n",
			   write_request);
	}
}

static enum plane_index decode_plane_index(struct kobj_attribute* attr) {
	enum plane_index idx;
	if (strcmp(attr->attr.name, "cpu") == 0) {
		idx = PLANE_INDEX_CPU;
	} else if (strcmp(attr->attr.name, "gpu") == 0) {
		idx = PLANE_INDEX_GPU;
	} else if (strcmp(attr->attr.name, "cache") == 0) {
		idx = PLANE_INDEX_CACHE;
	} else if (strcmp(attr->attr.name, "system_agent") == 0) {
		idx = PLANE_INDEX_SYSTEM_AGENT;
	} else if (strcmp(attr->attr.name, "analog_io") == 0) {
		idx = PLANE_INDEX_ANALOG_IO;
	} else {
		idx = PLANE_INDEX_UNKNOWN;
	}
	return idx;
}

static ssize_t offsets_show(struct kobject* kobj, struct kobj_attribute* attr,
							char* buf) {
	enum plane_index idx = decode_plane_index(attr);
	if (idx == PLANE_INDEX_UNKNOWN) {
		pr_err(LOGHDR "Unknown plane index, how did we get here?\n");
		return -EINVAL;
	}

	intoff_t offset = read_voltage_offset(idx);

	return offset_int_to_mv_str(buf, PAGE_SIZE, offset);
}

static ssize_t offsets_store(struct kobject* kobj, struct kobj_attribute* attr,
							 const char* buf, size_t count) {
	enum plane_index idx = decode_plane_index(attr);
	if (idx == PLANE_INDEX_UNKNOWN) {
		pr_err(LOGHDR "Unknown plane index, how did we get here?\n");
		return -EINVAL;
	}

	intoff_t offset;
	int ret = offset_mv_str_to_int(&offset, buf, count);

	if (ret == UERR_RANGE) {
		pr_err(LOGHDR "Voltage offset is outside the valid range [-999, 999].");
		return -EINVAL;
	}

#ifdef LOCK_OVERVOLT
	if (offset > 0 || ret == UERR_OVERVOLT) {
		pr_err(LOGHDR "Attempted overvolt, aborting...\n");
		return -EINVAL;
	}
#endif

	if (ret) {
		pr_err(LOGHDR "Invalid offset parameter\n");
		return -EINVAL;
	}

	write_voltage_offset(idx, offset);
	return count;
}

static struct kobj_attribute cpu_attribute =
	__ATTR(cpu, 0664, offsets_show, offsets_store);

static struct kobj_attribute gpu_attribute =
	__ATTR(gpu, 0664, offsets_show, offsets_store);

static struct kobj_attribute cache_attribute =
	__ATTR(cache, 0664, offsets_show, offsets_store);

static struct kobj_attribute system_agent_attribute =
	__ATTR(system_agent, 0664, offsets_show, offsets_store);

static struct kobj_attribute analog_io_attribute =
	__ATTR(analog_io, 0664, offsets_show, offsets_store);

// static struct kobj_attribute digital_io_attribute =
// 	__ATTR(digital_io, 0664, offsets_show, offsets_store);

static struct attribute* offsets_attrs[] = {
	&cpu_attribute.attr,	   &gpu_attribute.attr,
	&cache_attribute.attr,	   &system_agent_attribute.attr,
	&analog_io_attribute.attr, NULL,
};

static const struct attribute_group offsets_attr_group = {
	.attrs = offsets_attrs,
};

static struct kobject* offsets_kobj;

#ifdef TESTS
int run_msr_tests(void);
#endif // TESTS

static int __init kundervolt_init(void) {
	int ret = 0;

	pr_info(LOGHDR "Initializing module\n");

	// Verify this is a supported CPU
	unsigned int cpu = 0;
	struct cpuinfo_x86* cpuinfo;
	for_each_online_cpu(cpu) {
		cpuinfo = &cpu_data(cpu);
		if (cpuinfo->x86_vendor != X86_VENDOR_INTEL) {
			pr_err(LOGHDR "This module only works on Intel CPUs\n");
			return -EPERM;
		}
	}

#ifdef TESTS
	run_fp_tests();
	run_msr_tests();
#endif // TESTS

	offsets_kobj = kobject_create_and_add("offsets", &THIS_MODULE->mkobj.kobj);
	if (!offsets_kobj) {
		return -ENOMEM;
	}

	ret = sysfs_create_group(offsets_kobj, &offsets_attr_group);
	if (ret) {
		kobject_put(offsets_kobj);
		pr_err(LOGHDR "Failed to create undervolt offsets sysfs files");
		return ret;
	}

	pr_info(LOGHDR "Module ready!\n");
	return ret;
}

static void __exit kundervolt_exit(void) {
	pr_info(LOGHDR "Removing module!\n");
	sysfs_remove_group(offsets_kobj, &offsets_attr_group);
	kobject_put(offsets_kobj);
}

module_init(kundervolt_init);
module_exit(kundervolt_exit);

#ifdef TESTS

static int build_msr_value_test1(void) {
	uint64_t value =
		build_msr_request(PLANE_INDEX_CPU, MSR_OP_READ, 0xECC00000);
	ASSERT_EQ_LHEX(value, 0x80000010ecc00000ull);
	return 0;
}

static int build_msr_value_test2(void) {
	uint64_t value =
		build_msr_request(PLANE_INDEX_GPU, MSR_OP_WRITE, 0xF0000000);
	ASSERT_EQ_LHEX(value, 0x80000111F0000000ull);
	return 0;
}

static int build_msr_value_test3(void) {
	uint64_t value =
		build_msr_request(PLANE_INDEX_CACHE, MSR_OP_READ, 0xF9A00000);
	ASSERT_EQ_LHEX(value, 0x80000210F9A00000ull);
	return 0;
}

static int build_msr_value_test4(void) {
	uint64_t value =
		build_msr_request(PLANE_INDEX_SYSTEM_AGENT, MSR_OP_WRITE, 0);
	ASSERT_EQ_LHEX(value, 0x8000031100000000ull);
	return 0;
}

static int build_msr_value_test5(void) {
	uint64_t value =
		build_msr_request(PLANE_INDEX_ANALOG_IO, MSR_OP_READ, 0x09a00000);
	ASSERT_EQ_LHEX(value, 0x8000041009a00000ull);
	return 0;
}

// static int build_msr_value_test6(void) {
// 	uint64_t value =
// 		build_msr_request(PLANE_INDEX_DIGITAL_IO, MSR_OP_WRITE, 0xFFFFFFFF);
// 	ASSERT_EQ_LHEX(value, 0x80000511FFFFFFFFull);
// 	return 0;
// }

int run_msr_tests(void) {
	INIT_TEST_SUITE("Voltage MSR")

	RUN_TEST(build_msr_value_test1)
	RUN_TEST(build_msr_value_test2)
	RUN_TEST(build_msr_value_test3)
	RUN_TEST(build_msr_value_test4)
	RUN_TEST(build_msr_value_test5)
	// RUN_TEST(build_msr_value_test6)

	END_TEST_SUITE
}

#endif // TESTS
