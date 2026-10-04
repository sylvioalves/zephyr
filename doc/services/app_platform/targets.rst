.. _app_platform_targets:

Supported Targets
#################

The platform is written against Zephyr kernel interfaces only: memory domains,
user-mode threads, system calls, the task watchdog and the linkable loadable
extension subsystem. Any target that offers those can run it. The targets below
are the ones the platform is built and exercised on, by the test suites under
:zephyr_file:`tests/subsys/app_platform` and by the samples under
``samples/subsys/app_*``.

What a target needs
*******************

* :kconfig:option:`CONFIG_TASK_WDT`, which the platform depends on.
* For isolation, memory protection hardware supported by
  :kconfig:option:`CONFIG_USERSPACE`: an Arm MPU, a RISC-V PMP or an MMU. A
  target without it runs the platform in supervisor mode, see
  :ref:`app_platform`.
* For loadable modules, :kconfig:option:`CONFIG_LLEXT`, a PSA crypto provider
  for SHA-256 and ECDSA P-256, and a storage partition with a file system for
  the registry and the installed modules.
* For :kconfig:option:`CONFIG_APP_PLATFORM_CPU_AFFINITY`, an SMP kernel.

Supported and tested targets
****************************

.. list-table::
   :header-rows: 1
   :widths: 30 20 15 35

   * - Board
     - Architecture
     - User mode
     - Notes
   * - ``esp32c5_devkitc/esp32c5/hpcore``
     - RISC-V
     - Yes, PMP
     - Espressif ESP32-C5. Wi-Fi, BLE, 802.15.4 and external RAM services,
       loadable modules from flash storage.
   * - ``esp32c6_devkitc/esp32c6/hpcore``
     - RISC-V
     - Yes, PMP
     - Espressif ESP32-C6. Wi-Fi, BLE and 802.15.4 services, loadable modules
       from flash storage.
   * - ``esp32p4x_function_ev_board/esp32p4/hpcore``
     - RISC-V
     - Yes, PMP
     - Espressif ESP32-P4. Display and hardware counter services, loadable
       modules from flash storage.
   * - ``esp32_devkitc/esp32/procpu``
     - Xtensa
     - No
     - Espressif ESP32. Applications run as supervisor threads, see
       :ref:`app_platform`.
   * - ``qemu_riscv32/qemu_virt_riscv32``
     - RISC-V
     - Yes, PMP
     - Single core emulated target.
   * - ``qemu_riscv32/qemu_virt_riscv32/smp``
     - RISC-V
     - Yes, PMP
     - Two cores. Applications run on both CPUs, with per-application CPU
       affinity.
   * - ``mps2/an385``
     - Arm Cortex-M3
     - Yes, Armv7-M MPU
     - Emulated. The module development kit and the module interface are
       exercised here.
   * - ``mps2/an521/cpu0``
     - Arm Cortex-M33
     - Yes, Armv8-M MPU
     - Emulated.
   * - ``mps3/corstone300/an547``
     - Arm Cortex-M55
     - Yes, Armv8.1-M MPU
     - Emulated. An MPU with more regions.
   * - ``qemu_cortex_a53``
     - Arm AArch64
     - Yes, MMU
     - Emulated. The module development kit and the module interface are
       exercised here.
   * - ``qemu_cortex_a53/qemu_cortex_a53/smp``
     - Arm AArch64
     - Yes, MMU
     - Two cores, with per-application CPU affinity.
   * - ``qemu_x86``
     - x86
     - Yes, MMU
     - Emulated. Built-in applications, the device services, the heap,
       worker threads and the display service.

Every target with user mode also builds the platform with
:kconfig:option:`CONFIG_USERSPACE` disabled, and the same signed module runs in
both builds, see :ref:`app_platform_abi_stability`.

Cost when the platform is disabled
**********************************

With :kconfig:option:`CONFIG_APP_PLATFORM` disabled, no platform code is built.
The platform relies on changes to shared code, though, and those are built into
every firmware that enables the subsystem they belong to:

* Memory domains: a partition the architecture refuses is removed from the
  domain again, and the index of a released thread returns to the pool. Every
  build with :kconfig:option:`CONFIG_USERSPACE`.
* Arm MPU: :kconfig:option:`CONFIG_ARM_MPU` selects
  :kconfig:option:`CONFIG_ARCH_MEM_DOMAIN_SYNCHRONOUS_API`, so a change to the
  domain of the running thread takes effect at once rather than at its next
  context switch. Every Arm MPU build with :kconfig:option:`CONFIG_USERSPACE`.
* RISC-V PMP: adjacent domain partitions share entries, a partition that does
  not fit is refused, domain changes reach the other CPUs, and user entries are
  cleared on entry to kernel mode. Every RISC-V build with
  :kconfig:option:`CONFIG_USERSPACE`.
* LLEXT: the region copy is rewritten, code bound for non-executable RISC-V
  memory is refused, and the integer division helpers of the compiler run-time
  are exported in the ``LIBC`` symbol group, which links them into every image
  with :kconfig:option:`CONFIG_LLEXT`.
* The common C library: an arena too small for a heap leaves the heap
  uninitialized and allocations fail.

The table gives the growth these changes cause in firmware without the
platform, in bytes of code and read-only data, with the growth of zero-initialized
RAM in parentheses where there is any. Each figure compares the same build with
and without the changes.

.. list-table::
   :header-rows: 1
   :widths: 31 23 23 23

   * - Board
     - :zephyr:code-sample:`hello_world`
     - ``tests/kernel/mem_protect/userspace``
     - :zephyr:code-sample:`llext-shell-loader`
   * - ``mps2/an385``, Armv7-M MPU
     - 4
     - 464
     - 296
   * - ``qemu_riscv32``, RISC-V PMP
     - 12
     - 536 (80)
     - 458
   * - ``qemu_cortex_a53``, AArch64 MMU
     - 0
     - 94
     - 0

The user mode figures are mostly the memory domain hooks of each architecture,
and the LLEXT ones mostly the exported division helpers and the region copy.
The figures cover size only, not execution time.

Board files
***********

A board describes the devices the platform hands to applications by index in a
``zephyr,app-platform`` devicetree node, see :ref:`app_platform`, and the buses
and peripherals some services use through aliases and chosen nodes, see
:ref:`app_platform_services_catalog`. The overlays under
:zephyr_file:`tests/subsys/app_platform/boards` and the ``boards`` directory of
each sample show the settings each target uses, such as a larger storage
partition for the application store or the counter instances granted to
applications.
