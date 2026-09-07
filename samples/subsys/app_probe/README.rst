.. zephyr:code-sample:: app-probe
   :name: Application service probe

   Call every capability-gated service from a signed loadable application.

Overview
********

A signed application is verified, loaded as an extension and launched in user
mode. From there it calls each service the platform offers and reports what
happened, which makes the capability model visible: a service the application
was not granted answers ``-EPERM``, and one whose capability it holds but whose
specific resource it was not assigned answers ``-EACCES``.

The probe is the quickest way to see which services a given board actually
backs with hardware. A service with no device behind it answers ``-ENODEV``
without revealing whether the peripheral exists.

Requirements
************

A board with userspace support and a storage partition for the signed module.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_probe
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

The build signs the module with a development key generated into the sample's
``keys`` directory, which is deliberately not tracked.

See :ref:`app_platform` for the capability model.
