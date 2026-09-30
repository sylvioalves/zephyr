.. zephyr:code-sample:: app-persist
   :name: Persistent signed application

   Install a signed application to flash and run it again after a reset.

Overview
********

Stores a signed application and its manifest in flash, then verifies and runs
it on every boot. Verification is two steps: the signature authenticates the
manifest, and the module is accepted only if its hash matches the one the
authenticated manifest carries. The sample also demonstrates the checks that
must fail, tampering with the stored bytes so the loader rejects them, and
offering an older version so the registry refuses the downgrade.

A manifest stored by a firmware that used the previous manifest format is
refused after an update; erase the storage partition so the sample installs the
application again.

Requirements
************

A board with userspace support and a filesystem partition.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_persist
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

Reset the board after the first run: the application is loaded from flash and
starts again without being uploaded.

See :ref:`app_platform` for how a loaded application is verified.
