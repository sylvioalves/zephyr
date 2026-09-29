.. zephyr:code-sample:: app-workers

   :name: Application worker threads

   Run one application on two threads that hand items to each other.

Overview
********

Launches an application whose entry thread starts a worker with
``app_svc_thread_spawn`` and exchanges five items with it through two semaphores
from ``app_svc_sem_init``. The worker produces each item into memory both
threads reach, and the entry thread consumes it, then waits for the worker with
``app_svc_thread_join`` and returns, which ends the application.

The platform reserves the worker's stack at build time through
:kconfig:option:`CONFIG_APP_PLATFORM_APP_THREADS`, and the application needs
``APP_CAP_THREAD`` to use it. The sample also runs without
:kconfig:option:`CONFIG_USERSPACE`.

Building and running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_workers
   :board: qemu_x86
   :goals: build run
   :compact:

Sample output
*************

.. code-block:: console

   I: [app 0] consumer got 1
   I: [app 0] consumer got 4
   I: [app 0] consumer got 9
   I: [app 0] consumer got 16
   I: [app 0] consumer got 25
   done: 2 threads of one app passed 5 items, sum 55
