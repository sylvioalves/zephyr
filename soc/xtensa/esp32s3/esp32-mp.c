#include <ksched.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/kernel_structs.h>

#include <soc.h>
#include <esp_cpu.h>
#include <hal/soc_hal.h>
#include <hal/soc_ll.h>
#include <zephyr/drivers/interrupt_controller/intc_esp32.h>

static struct k_spinlock loglock;

/* Note that the logging done here is ACTUALLY REQUIRED FOR RELIABLE
 * OPERATION!  At least one particular board will experience spurious
 * hangs during initialization (usually the APPCPU fails to start at
 * all) without these calls present.  It's not just time -- careful
 * use of k_busy_wait() (and even hand-crafted timer loops using the
 * Xtensa timer SRs directly) that duplicates the timing exactly still
 * sees hangs.  Something is happening inside the ROM UART code that
 * magically makes the startup sequence reliable.
 *
 * Leave this in place until the sequence is understood better.
 *
 * (Note that the use of the spinlock is cosmetic only -- if you take
 * it out the messages will interleave across the two CPUs but startup
 * will still be reliable.)
 */
void smp_log(const char *msg)
{
	while (*msg) {
		esp_rom_uart_tx_one_char(*msg++);
	}
	esp_rom_uart_tx_one_char('\r');
	esp_rom_uart_tx_one_char('\n');
}

/* The calls and sequencing here were extracted from the ESP-32
 * FreeRTOS integration with just a tiny bit of cleanup.  None of the
 * calls or registers shown are documented, so treat this code with
 * extreme caution.
 */
void start_other_core(void *entry_point)
{
	soc_ll_unstall_core(1);

	if (!REG_GET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN)) {
		REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
		REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
		REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETTING);
		REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETTING);
	}

	esp_rom_ets_set_appcpu_boot_addr((void *)entry_point);

	ets_delay_us(50000);

	// esp_rom_uart_tx_one_char('\r');
	// esp_rom_uart_tx_one_char('\r');
	// esp_rom_uart_tx_one_char('\n');

	// smp_log("ESP32S3: CPU1 start sequence complete");
}
