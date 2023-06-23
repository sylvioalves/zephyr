#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include "hal/cpu_ll.h"
#define THREADS_NUM	2

#define STACK_SIZE	1024
static K_THREAD_STACK_ARRAY_DEFINE(tstack, THREADS_NUM, STACK_SIZE);
static struct k_thread tthread[THREADS_NUM];

static int curr_cpu(void)
{
	unsigned int k = arch_irq_lock();
	int ret = cpu_ll_get_core_id();
	arch_irq_unlock(k);
	return ret;
}

void test_thread(void *arg1, void *arg2, void *arg3)
{
    const uint32_t counter = *(uint32_t*)arg1;
    printk("Staring thread number %d\n", counter);
    while(1)
    {
        uint32_t proc_id = cpu_ll_get_core_id();
        printk("Thread number %d running on cpu %d\n", counter, proc_id);
        k_sleep(K_SECONDS(1));
    }
}

int main() {
    uint32_t proc_id = cpu_ll_get_core_id();
    printk("Main running on cpu %d\n", proc_id);
    for (uint32_t i = 0; i < THREADS_NUM; i++) {

		k_thread_create(&tthread[i], tstack[i], STACK_SIZE,
			(k_thread_entry_t)test_thread, (void *)&i, NULL, NULL,
			K_PRIO_COOP(11) , 0, K_FOREVER);

		// k_thread_cpu_mask_clear(&tthread[i]);
		// k_thread_cpu_mask_enable(&tthread[i], i);
		k_thread_start(&tthread[i]);
	}

    while(1)
    {
        k_yield();
    }
    return 0;
}
