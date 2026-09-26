/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/arch/arch_interface.h>

#define GRAN       MAX(CONFIG_PMP_GRANULARITY, 64)
#define CHUNK      (3 * GRAN)
#define NUM_CHUNKS 8
#define STACK_SIZE 1024

static uint8_t __aligned(4096) area[2 * NUM_CHUNKS * CHUNK];

static struct k_mem_partition parts[NUM_CHUNKS];
static struct k_mem_domain domain;
static int num_parts;
static size_t stride;

K_THREAD_STACK_DEFINE(user_stack, STACK_SIZE);
static struct k_thread user_thread;

static void touch_parts(void *p1, void *p2, void *p3)
{
	int count = POINTER_TO_INT(p1);
	size_t step = POINTER_TO_UINT(p2);
	uint32_t writable = POINTER_TO_UINT(p3);

	for (int i = 0; i < count; i++) {
		volatile uint8_t *chunk = &area[i * step];

		if ((writable & BIT(i)) != 0U) {
			chunk[0] = (uint8_t)(i + 1);
			chunk[CHUNK - 1] = (uint8_t)(i + 1);
		} else {
			(void)chunk[0];
			(void)chunk[CHUNK - 1];
		}
	}
}

/* Run a user thread in the domain and return the PMP entries its partitions took. */
static unsigned int run_in_domain(void)
{
	uint32_t writable = 0U;
	k_tid_t tid;

	for (int i = 0; i < num_parts; i++) {
		if ((parts[i].attr.pmp_attr & PMP_W) != 0U) {
			writable |= BIT(i);
		}
	}

	tid = k_thread_create(&user_thread, user_stack, STACK_SIZE, touch_parts,
			      INT_TO_POINTER(num_parts), UINT_TO_POINTER(stride),
			      UINT_TO_POINTER(writable), 5, K_USER, K_FOREVER);

	zassert_ok(k_mem_domain_add_thread(&domain, tid));
	k_thread_start(tid);
	zassert_ok(k_thread_join(tid, K_FOREVER));

	return user_thread.arch.u_mode_pmp_end_index - user_thread.arch.u_mode_pmp_domain_offset;
}

static int add_chunk(int i, k_mem_partition_attr_t attr)
{
	parts[i] = (struct k_mem_partition){
		.start = (uintptr_t)&area[i * stride],
		.size = CHUNK,
		.attr = attr,
	};
	return k_mem_domain_add_partition(&domain, &parts[i]);
}

static void check_written(void)
{
	for (int i = 0; i < num_parts; i++) {
		if ((parts[i].attr.pmp_attr & PMP_W) != 0U) {
			zassert_equal(area[i * stride], i + 1, "chunk %d not written", i);
			zassert_equal(area[i * stride + CHUNK - 1], i + 1,
				      "chunk %d end not written", i);
		}
	}
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);

	memset(area, 0, sizeof(area));
	num_parts = 0;
	zassert_ok(k_mem_domain_init(&domain, 0, NULL));
}

ZTEST(pmp_domain_merge, test_contiguous_partitions_merge)
{
	int count = MIN(NUM_CHUNKS, arch_mem_domain_max_partitions_get());

	stride = CHUNK;
	for (int i = 0; i < count; i++) {
		zassert_ok(add_chunk(i, K_MEM_PARTITION_P_RW_U_RW), "chunk %d refused", i);
		num_parts++;
	}

	zassert_equal(run_in_domain(), 2, "contiguous partitions were not merged");
	check_written();
}

ZTEST(pmp_domain_merge, test_adjacent_partitions_share_a_bound)
{
	stride = CHUNK;
	zassert_ok(add_chunk(1, K_MEM_PARTITION_P_RW_U_RO));
	zassert_ok(add_chunk(0, K_MEM_PARTITION_P_RW_U_RW));
	num_parts = 2;

	zassert_equal(run_in_domain(), 3, "adjacent partitions did not share a bound");
	check_written();

	/* The kernel may still write what the user thread could only read. */
	area[stride] = 0x55;
	zassert_equal(area[stride], 0x55);
}

ZTEST(pmp_domain_merge, test_partition_that_does_not_fit_is_refused)
{
	int rc = 0;

	stride = 2 * CHUNK;
	for (int i = 0; i < NUM_CHUNKS; i++) {
		rc = add_chunk(i, K_MEM_PARTITION_P_RW_U_RW);
		if (rc != 0) {
			break;
		}
		num_parts++;
	}

	zassert_equal(rc, -ENOSPC, "no partition was refused (rc %d)", rc);
	zassert_equal(domain.num_partitions, num_parts, "refused partition was kept");
	zassert_equal(run_in_domain(), 2 * num_parts);
	check_written();
}

ZTEST_SUITE(pmp_domain_merge, NULL, NULL, before_each, NULL, NULL);
