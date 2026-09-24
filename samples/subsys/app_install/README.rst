.. _app_install_sample:

Over-the-wire app install
#########################

Overview
********

Upload a signed application to the device over a single serial cable and
launch it as an isolated user-mode app. The device exposes a shell on its
console UART; the same UART carries the mcumgr SMP transport, so files can be
uploaded without a second port or losing the console.

The application is signed on the build host. Only the public key is compiled
into the firmware, so the device accepts only an app signed by the matching
private key. The upload lands two files in the device's storage:

* ``/lfs/<name>.llext.new``     - the module
* ``/lfs/<name>.manifest.new``  - the signed manifest (name, version, module
                                  hash) followed by its signature

The shell command ``app install <name>`` then verifies the manifest, binds the
module to it, moves both over the installed copies (refusing a version older
than the installed one), records it in the registry, and launches it - streamed from
flash, never held whole in RAM.

Building and running
********************

.. code-block:: console

   west build -b esp32c5_devkitc/esp32c5/hpcore samples/subsys/app_install
   west flash

Host workflow
*************

Prepare the two files (the build emits the module and the manifest bytes), then
upload and install. ``smpmgr`` drives the SMP transport over serial:

.. code-block:: console

   smpmgr --port /dev/ttyUSB1 file upload app.manifest /lfs/myapp.manifest.new
   smpmgr --port /dev/ttyUSB1 file upload app.llext    /lfs/myapp.llext.new

On the device shell:

.. code-block:: console

   uart:~$ app install myapp
   installed and launched 'myapp' v1 (isolated app 0)
   uart:~$ app list
     myapp  v1  (autostart)

A tampered upload is rejected: if the module bytes do not match the signed
manifest hash, ``app install`` fails and the app is never launched.
