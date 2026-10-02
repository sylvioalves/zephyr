.. zephyr:code-sample:: app-can
   :name: Isolated CAN application

   Send and receive CAN frames from a user-mode application.

Overview
********

Launches an application that sends CAN frames and reads them back through
platform services, never touching the controller itself. The platform owns the
controller, and the sample sets :kconfig:option:`CONFIG_APP_PLATFORM_CAN_LOOPBACK`
so it runs in loopback and the frames come back on a single board without a
transceiver. A payload longer
than a classic CAN frame is refused rather than truncated.

A second application is launched without ``APP_CAP_CAN``. It calls the same two
services and both answer ``-EPERM``, showing that the capability, not the
presence of the hardware, decides who may use the bus.

Requirements
************

A board with a ``zephyr,canbus`` chosen node and userspace support.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_can
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

See :ref:`app_platform_policy` for why a peripheral is exposed as a service
rather than as a device handle.
