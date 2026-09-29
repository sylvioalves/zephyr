.. zephyr:code-sample:: app-heap
   :name: Per-application heap

   Allocate from a heap the platform maps into one application.

Overview
********

Launches an application that declares a 1024-byte heap in its catalog entry. The
platform carves the heap from the pool reserved by
:kconfig:option:`CONFIG_APP_PLATFORM_APP_HEAP_POOL_SIZE`, and with
:kconfig:option:`CONFIG_USERSPACE` maps it into the application's memory domain.
The application builds a linked list of ten nodes with ``app_malloc``, sums the
values, frees every node with ``app_free``, and then allocates half the heap in
one block to show the freed nodes were merged back. The allocator runs in the
application's own thread. The sample also runs without
:kconfig:option:`CONFIG_USERSPACE`.

Building and running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_heap
   :board: qemu_x86
   :goals: build run
   :compact:

Sample output
*************

.. code-block:: console

   heap region of 4096 bytes for 1024 requested
   I: [app 0] list of 10 nodes, sum 385
   done: a list of 10 nodes on the app heap, sum 385, heap reusable

The region size depends on the memory protection hardware: whole pages with an
MMU, a power of two with an MPU or PMP.
