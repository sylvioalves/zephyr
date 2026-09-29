.. zephyr:code-sample:: app-ring

   :name: Isolated bulk-transfer ring application

   Move a batch of bytes through a ring mapped into the application.

Overview
********

Launches an application that binds a ring buffer, writes into it directly and
sends the whole batch with one call. Filling the ring costs no syscall at all,
because the platform mapped the buffer into the application's own memory
domain, so a payload of any size crosses the boundary once instead of once per
buffer.

The sample sends the same payload both ways on the same port and prints the
cycle count for each, so the benefit is measured on the board rather than
assumed. On an ESP32-C5 at 240MHz, 192 bytes cost about 87k cycles through the
ring against about 266k through repeated ``app_svc_uart_write`` calls.

Because the ring's indices live in application memory, an application can
rewrite them at any moment. The sample plants an out-of-range head and tail and
checks that the platform reports ``-EBADMSG`` rather than following them, which
is why the indices are range-checked on every use instead of trusted.

Requirements
************

A board whose devicetree chooses ``app-uart``, and userspace support. The
supplied overlay does this for ``esp32c5_devkitc`` and loops the port back
through the pin matrix.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_ring
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

Each check prints ``PASS`` or ``FAIL`` with the value it observed.

See :ref:`app_platform_policy` for why a resource is exposed scoped rather than
whole.
