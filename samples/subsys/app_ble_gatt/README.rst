.. zephyr:code-sample:: app-ble-gatt
   :name: Isolated GATT application

   Read and write a BLE peer's characteristics by UUID.

Overview
********

Launches an application that names a GATT service and characteristic by UUID
and lets the platform connect, discover and perform the exchange. No connection
or attribute handle ever reaches the application, so it can address only what it
names, and only within a service it was granted.

The peer exposes two services. The application is granted the sensor service
and not the private one, so reading and writing the sensor service succeeds
while a read of the private service is refused with ``-EACCES`` before any GATT
traffic is generated. A second application launched without ``APP_CAP_BLE`` is
refused with ``-EPERM``, which separates holding the capability from being
granted a service.

Requirements
************

Two boards with BLE support. Build ``peer`` for one and the sample for the
other.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_ble_gatt
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

Build and flash the peer to the second board, reset it first, then reset the
device:

.. code-block:: console

   west build -b esp32c3_devkitc samples/subsys/app_ble_gatt/peer

See :ref:`app_platform_policy` for why a peripheral is exposed as a service
rather than as a device handle.
