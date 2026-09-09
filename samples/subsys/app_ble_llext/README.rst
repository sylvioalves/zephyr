.. zephyr:code-sample:: app-ble-llext
   :name: GATT server in a loadable application

   Serve a GATT service from a loadable user-mode application.

Overview
********

A loadable extension is verified against its SHA-256 digest, launched in user
mode and granted one service UUID. From there it serves a GATT service: a
central discovers the service, reads a counter the application publishes and
writes to a second characteristic, which the application receives as an event.

The module links no Bluetooth code. It calls ``svc_ble_serve_start()`` to
publish the service, ``svc_ble_serve_set()`` to update a value and takes a
peer's write through the dispatch loop, so the host owns the connection, the
attribute table and the value buffers throughout. The application never sees a
connection object or an attribute handle.

The counter characteristic is read-only and carries a value the application
updates once a second. The second characteristic is writable, and each write
is logged with the byte the peer sent.

Requirements
************

A board with userspace and Bluetooth support.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_ble_llext
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

Connect with a phone or any other central and discover the service to read the
counter and write to the inbox characteristic.

See :ref:`app_platform` for the capability model.
