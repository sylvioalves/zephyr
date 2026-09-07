.. zephyr:code-sample:: app-io
   :name: Isolated I/O application

   Give a user-mode application one granted LED and button.

Overview
********

Launches an application that blinks an LED and waits on a button through
platform services, never touching the hardware itself. The application is
granted exactly one LED pin, so a request for any other answers ``-EACCES``
even though it holds the LED capability. That split between holding a
capability and being assigned a resource is the point of the sample.

Requirements
************

A board with an ``led0`` alias and a ``sw0`` button, and userspace support.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_io
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

See :ref:`app_platform_policy` for why a bus or pin is exposed scoped rather
than whole.
