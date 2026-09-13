.. zephyr:code-sample:: app-timer-slots

   :name: Isolated multi-timer application

   Run several platform timers at once from one user-mode application.

Overview
********

Launches an application that claims two of its timer slots, arms them for
different intervals and waits on each separately, so it can tell which one
fired. It then checks the periodic mode, the elapsed count, and that releasing
a slot returns it to the application's pool until every slot is claimable
again and a further request reports ``-ENOSPC``.

Each slot is a platform object reached through a capability-gated syscall, so
the application runs several timers without owning a kernel timer or being
able to affect another application's. The slots are a fixed per-application
array rather than allocations, and the platform returns them when the
application exits or faults.

Requirements
************

A board with userspace support.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_timer_slots
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

Each check prints ``PASS`` or ``FAIL`` with the value it observed, followed by
a count, so a run either shows a clean sheet or names the call that broke.

See :ref:`app_platform_policy` for why a resource is exposed scoped rather
than whole.
