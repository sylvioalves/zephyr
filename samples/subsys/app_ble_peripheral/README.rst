.. zephyr:code-sample:: app-ble-peripheral
   :name: Isolated GATT server application

   Serve a GATT service from a user-mode application.

Overview
********

Launches an application that declares a GATT service and two characteristics by
UUID and lets the platform serve them. The platform registers the service,
advertises, owns the connection and answers reads from values the application
publishes, so a phone or any other central can connect to the board and talk to
the application.

A peer's write arrives as an ``APP_EVENT_GATT_WRITE`` input event, dispatched to
a handler that runs on the application's own thread in user mode. Nothing calls
from the Bluetooth stack into application code, and no connection or attribute
handle ever reaches the application.

One characteristic carries a counter the application updates; the other is
writable, and the application logs what a peer wrote to it.

Requirements
************

A board with BLE support and userspace. A second board or a phone acts as the
central. ``central/`` holds a central that drives the sample without a phone.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_ble_peripheral
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

See :ref:`app_platform_policy` for why a peripheral is exposed as a service
rather than as a device handle.
