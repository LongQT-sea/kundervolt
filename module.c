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
#include <linux/mutex.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include <asm/msr.h>
#include <asm/processor.h>

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

/*
 * Serialises access to the voltage mailbox.
 *
 * MSR 0x150 is a single request/response register: reading an offset means
 * writing a read request and then reading the reply back. Two concurrent users
 * would interleave and hand each other the wrong plane's value.
 */
static DEFINE_MUTEX(msr_lock);

/*
 * One representative CPU per physical package.
 *
 * The mailbox is package scoped, and wrmsrq_safe() only ever acts on the CPU it
 * happens to run on. On a multi socket board that means a plain write would
 * undervolt whichever package the scheduler picked and silently leave the
 * others alone. Only mutated at init; read under msr_lock.
 */
static cpumask_var_t package_cpus;

struct msr_xfer {
	uint64_t request;
	uint64_t result;
	int err;
};

static inline uint64_t build_msr_request(enum plane_index idx,
										 enum msr_operation op,
										 intoff_t offset) {
	uint64_t extended_offset = offset;
	return MSR_VOLTAGE_BASE_VALUE | idx | op |
		   (extended_offset & MSR_VOLTAGE_OFFSET_MASK);
}

/*
 * Both helpers below run via smp_call_function_single(), which executes them on
 * the target CPU with interrupts disabled. That is what makes the write/read
 * pair in msr_mailbox_read() atomic with respect to that CPU: without it the
 * task could be migrated between the two MSR accesses and read the reply from a
 * different package than it sent the request to.
 */
static void msr_mailbox_read(void* info) {
	struct msr_xfer* xfer = info;

	xfer->err = wrmsrq_safe(MSR_ADDR_VOLTAGE, xfer->request);
	if (xfer->err) {
		return;
	}
	xfer->err = rdmsrq_safe(MSR_ADDR_VOLTAGE, &xfer->result);
}

static void msr_mailbox_write(void* info) {
	struct msr_xfer* xfer = info;

	xfer->err = wrmsrq_safe(MSR_ADDR_VOLTAGE, xfer->request);
}

static int read_voltage_offset(enum plane_index idx, intoff_t* out) {
	struct msr_xfer xfer = {
		.request = build_msr_request(idx, MSR_OP_READ, 0),
	};
	unsigned int cpu;
	int ret;

	lockdep_assert_held(&msr_lock);
	lockdep_assert_cpus_held();

	// The planes are package scoped, so any one package answers for all of them
	cpu = cpumask_first(package_cpus);
	if (cpu >= nr_cpu_ids) {
		return -ENODEV;
	}

	ret = smp_call_function_single(cpu, msr_mailbox_read, &xfer, 1);
	if (ret) {
		pr_err(LOGHDR "Could not run mailbox read on cpu %u: %d\n", cpu, ret);
		return ret;
	}
	if (xfer.err) {
		pr_err(LOGHDR "Mailbox read rejected on cpu %u (request %llx)\n", cpu,
			   xfer.request);
		return -EIO;
	}

	// The reply occupies the low 32 bits of the mailbox register
	*out = (intoff_t)(uint32_t)xfer.result;
	return 0;
}

static int write_voltage_offset(enum plane_index idx, intoff_t offset) {
	struct msr_xfer xfer = {
		.request = build_msr_request(idx, MSR_OP_WRITE, offset),
	};
	unsigned int cpu;
	int ret;

	lockdep_assert_held(&msr_lock);
	lockdep_assert_cpus_held();

#ifdef LOCK_OVERVOLT
	if (offset > 0) {
		return -EINVAL;
	}
#endif

	for_each_cpu(cpu, package_cpus) {
		ret = smp_call_function_single(cpu, msr_mailbox_write, &xfer, 1);
		if (ret) {
			pr_err(LOGHDR "Could not run mailbox write on cpu %u: %d\n", cpu,
				   ret);
			return ret;
		}
		if (xfer.err) {
			pr_err(LOGHDR "Mailbox write rejected on cpu %u (request %llx)\n",
				   cpu, xfer.request);
			return -EIO;
		}
	}

	return 0;
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
	intoff_t offset = 0;
	size_t written;
	int ret;

	if (idx == PLANE_INDEX_UNKNOWN) {
		pr_err(LOGHDR "Unknown plane index, how did we get here?\n");
		return -EINVAL;
	}

	// cpus_read_lock() keeps the package representative from going offline
	cpus_read_lock();
	mutex_lock(&msr_lock);
	ret = read_voltage_offset(idx, &offset);
	mutex_unlock(&msr_lock);
	cpus_read_unlock();

	/*
	 * Propagate the failure instead of reporting 0.00, which is exactly what an
	 * untouched CPU reads and would leave no way to tell "not supported" apart
	 * from "supported, currently at zero".
	 */
	if (ret) {
		return ret;
	}

	written = offset_int_to_mv_str(buf, PAGE_SIZE - 1, offset);
	written += scnprintf(buf + written, PAGE_SIZE - written, "\n");
	return written;
}

static ssize_t offsets_store(struct kobject* kobj, struct kobj_attribute* attr,
							 const char* buf, size_t count) {
	enum plane_index idx = decode_plane_index(attr);
	intoff_t offset = 0;
	intoff_t readback = 0;
	int ret;

	if (idx == PLANE_INDEX_UNKNOWN) {
		pr_err(LOGHDR "Unknown plane index, how did we get here?\n");
		return -EINVAL;
	}

	ret = offset_mv_str_to_int(&offset, buf, count);
	switch (ret) {
	case 0:
		break;
	case UERR_RANGE:
		pr_err(LOGHDR "Voltage offset is outside the valid range [%d, %d]\n",
			   VOLTAGE_RANGE_MIN, VOLTAGE_RANGE_MAX);
		return -EINVAL;
#ifdef LOCK_OVERVOLT
	case UERR_OVERVOLT:
		pr_err(LOGHDR "Attempted overvolt, aborting...\n");
		return -EINVAL;
#endif
	default:
		pr_err(LOGHDR "Invalid offset parameter\n");
		return -EINVAL;
	}

	cpus_read_lock();
	mutex_lock(&msr_lock);
	ret = write_voltage_offset(idx, offset);
	if (!ret) {
		ret = read_voltage_offset(idx, &readback);
	}
	mutex_unlock(&msr_lock);
	cpus_read_unlock();

	if (ret) {
		return ret;
	}

	/*
	 * Verify the offset actually landed. MSR 0x150 is the interface behind
	 * Plundervolt (CVE-2019-11157) and plenty of microcode and BIOS updates
	 * disable it outright, in which case the write is accepted and quietly
	 * ignored. This is only a warning rather than an error because the cpu and
	 * cache planes share a voltage in hardware, so a legitimate mismatch is
	 * possible when the two are set to different values.
	 */
	if (readback != offset) {
		pr_warn(LOGHDR
				"%s: requested offset 0x%08x but hardware reports 0x%08x. The "
				"offset may not have been applied (MSR 0x150 disabled by "
				"microcode or BIOS?) or is overridden by another plane.\n",
				attr->attr.name, (uint32_t)offset, (uint32_t)readback);
	}

	return count;
}

static struct kobj_attribute cpu_attribute =
	__ATTR(cpu, 0644, offsets_show, offsets_store);

static struct kobj_attribute gpu_attribute =
	__ATTR(gpu, 0644, offsets_show, offsets_store);

static struct kobj_attribute cache_attribute =
	__ATTR(cache, 0644, offsets_show, offsets_store);

static struct kobj_attribute system_agent_attribute =
	__ATTR(system_agent, 0644, offsets_show, offsets_store);

static struct kobj_attribute analog_io_attribute =
	__ATTR(analog_io, 0644, offsets_show, offsets_store);

// static struct kobj_attribute digital_io_attribute =
// 	__ATTR(digital_io, 0644, offsets_show, offsets_store);

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

// Picks the lowest numbered online CPU of every distinct physical package
static void collect_package_cpus(struct cpumask* dst) {
	unsigned int cpu;
	unsigned int rep;

	cpumask_clear(dst);
	for_each_online_cpu(cpu) {
		bool seen = false;

		for_each_cpu(rep, dst) {
			if (topology_physical_package_id(rep) ==
				topology_physical_package_id(cpu)) {
				seen = true;
				break;
			}
		}

		if (!seen) {
			cpumask_set_cpu(cpu, dst);
		}
	}
}

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
			return -ENODEV;
		}
	}

#ifdef TESTS
	run_fp_tests();
	run_msr_tests();
#endif // TESTS

	if (!alloc_cpumask_var(&package_cpus, GFP_KERNEL)) {
		return -ENOMEM;
	}

	cpus_read_lock();
	collect_package_cpus(package_cpus);
	cpus_read_unlock();

	if (cpumask_empty(package_cpus)) {
		pr_err(LOGHDR "Found no usable CPU packages\n");
		ret = -ENODEV;
		goto err_free_mask;
	}
	pr_info(LOGHDR "Found %u physical package(s)\n",
			cpumask_weight(package_cpus));

	offsets_kobj = kobject_create_and_add("offsets", &THIS_MODULE->mkobj.kobj);
	if (!offsets_kobj) {
		ret = -ENOMEM;
		goto err_free_mask;
	}

	ret = sysfs_create_group(offsets_kobj, &offsets_attr_group);
	if (ret) {
		pr_err(LOGHDR "Failed to create undervolt offsets sysfs files\n");
		goto err_put_kobj;
	}

	pr_info(LOGHDR "Module ready!\n");
	return 0;

err_put_kobj:
	kobject_put(offsets_kobj);
err_free_mask:
	free_cpumask_var(package_cpus);
	return ret;
}

static void __exit kundervolt_exit(void) {
	pr_info(LOGHDR "Removing module!\n");
	sysfs_remove_group(offsets_kobj, &offsets_attr_group);
	kobject_put(offsets_kobj);
	free_cpumask_var(package_cpus);
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
