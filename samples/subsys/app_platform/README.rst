.. zephyr:code-sample:: app-platform
   :name: Application platform

   Run applications as isolated, supervised user-mode groups.

Overview
********

Launches applications as separate groups. Each runs as a user-mode thread in
its own memory domain, holds only the capabilities it was granted, and is
reclaimed without disturbing the others if it faults or hangs.

The sample starts a built-in application, shows the platform's view of the
slots it occupies, and demonstrates that a fault in one group leaves the rest
running.

Requirements
************

A board with userspace support. The platform needs
:kconfig:option:`CONFIG_USERSPACE` and :kconfig:option:`CONFIG_TASK_WDT`, so
the target must have a memory protection unit.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_platform
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

See :ref:`app_platform` for the capability model and
:ref:`app_platform_policy` for how a subsystem is classified before it is
exposed.
