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
private key. The sample signs with the development key, generated under
``keys/`` on the first build and trusted through
:kconfig:option:`CONFIG_APP_PLATFORM_DEV_KEY`. The upload lands two files in the
device's storage:

* ``/lfs/<name>.llext.new``     - the module
* ``/lfs/<name>.manifest.new``  - the signed manifest (name, version, module
                                  hash) followed by its signature

The shell command ``store install <name>`` then verifies the manifest, binds the
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

The build signs the bundled reference app (named ``uploaded``) and writes it as an installable
bundle, ``build/bundle/uploaded.llext`` and ``build/bundle/uploaded.manifest``. ``west app
install`` uploads both files over the MCUmgr SMP transport carried by the shell and then runs
``store install`` on the device through the MCUmgr shell management group:

.. code-block:: console

   west app install -d build -p /dev/ttyUSB1

.. code-block:: console

   installing "uploaded" v1 from .../build/bundle
   uploaded uploaded.llext (2976 bytes) and uploaded.manifest
   device: installed and launched 'uploaded' v1 (isolated app 0)

The command exits non-zero with the device's error code when the device refuses the app.
``west app info -d build`` shows the id of the key that signed the bundle and the capabilities it
requests and requires. The same steps can be done by hand with any SMP client, followed by the
shell command:

.. code-block:: console

   smpmgr --port /dev/ttyUSB1 file upload uploaded.manifest /lfs/uploaded.manifest.new
   smpmgr --port /dev/ttyUSB1 file upload uploaded.llext    /lfs/uploaded.llext.new

.. code-block:: console

   uart:~$ store install uploaded
   installed and launched 'uploaded' v1 (isolated app 0)
   uart:~$ store list
     uploaded  v1  (autostart)

A tampered upload is rejected: if the module bytes do not match the signed
manifest hash, ``store install`` fails and the app is never launched.

Signing with a publisher key
****************************

The development key proves nothing about who built an app. To trust a publisher key instead,
list its public key in a key file, disable the development key and give the build the private
key, kept outside the tree, with its key id:

.. code-block:: yaml

   keys:
     - id: 1
       key: acme.pub.pem
       caps: [LOG, RANDOM]

.. code-block:: console

   west build -b esp32c5_devkitc/esp32c5/hpcore samples/subsys/app_install -- \
           -DCONFIG_APP_PLATFORM_DEV_KEY=n '-DCONFIG_APP_PLATFORM_KEYS_FILE="/secure/keys.yaml"' \
           -DAPP_SIGNING_KEY=/secure/acme.pem -DAPP_SIGNING_KEY_ID=1

The build refuses to sign with a key the key file does not list under that id. See
:ref:`app_platform` for the key file format.
