.. zephyr:code-sample:: app_edk
   :name: Application module development kit
   :relevant-api: app_platform_sdk

   Build and sign an application module outside the tree with the development kit of a firmware.

Overview
********

A firmware built with the application platform loads modules that a writer
builds without the Zephyr tree and without the firmware sources, from the
module development kit of the firmware. The kit holds the module SDK headers,
the module compiler flags of the target, the symbols a module may import, the
facts and public signing keys the firmware checks, the signer and a CMake
helper.

The firmware side of this sample:

* lists the publisher key it trusts in :file:`keys.yaml`, with key id 1, the
  capabilities the key may grant and the application names it may sign, through
  :kconfig:option:`CONFIG_APP_PLATFORM_KEYS_FILE`, and does not accept the
  development key;
* packs its kit with the ``llext-edk`` build target, the LLEXT EDK with the
  application module files of :kconfig:option:`CONFIG_APP_PLATFORM_EDK`;
* verifies the bundle of the module, ``hello.llext`` and ``hello.manifest``,
  against the key table, loads it and runs it.

The directory :file:`module` is the writer side: a CMake project that includes
only the kit and builds, checks and signs :file:`module/hello.c`.

The sample generates its publisher private key under :file:`keys` on the first
build. A product lists only the public key of each publisher in its key list.

Building and running
********************

By default the build packs the kit, extracts it into the build directory and
builds :file:`module` with it, without ``ZEPHYR_BASE`` in the environment of
that step, before it builds the firmware with the module:

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_edk
   :board: mps2/an385
   :goals: build run
   :compact:

Building the module outside the tree
************************************

The same steps by hand, as a module writer does them. Build the firmware and
its kit:

.. code-block:: console

   west build -b mps2/an385 samples/subsys/app_edk -d build
   west build -d build -t llext-edk

The kit is :file:`build/zephyr/app-platform-edk.tar.xz`. Extract it anywhere,
and build the module project with it, the toolchain the kit names in its
:file:`firmware.txt` and the private key of key id 1:

.. code-block:: console

   mkdir ~/edk && tar -C ~/edk -xf build/zephyr/app-platform-edk.tar.xz
   pip install -r ~/edk/app-platform-edk/scripts/requirements.txt
   cmake -S samples/subsys/app_edk/module -B ~/edk/hello -G Ninja \
         -DAPP_EDK_DIR=$HOME/edk/app-platform-edk \
         -DAPP_EDK_CROSS_COMPILE=$ZEPHYR_SDK_INSTALL_DIR/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi- \
         -DAPP_SIGNING_KEY=$PWD/samples/subsys/app_edk/keys/publisher.pem \
         -DAPP_SIGNING_KEY_ID=1
   cmake --build ~/edk/hello

Then rebuild the firmware with that bundle and run it:

.. code-block:: console

   west build -d build -- -DAPP_EDK_BUNDLE_DIR=$HOME/edk/hello
   west build -d build -t run

Sample output
=============

.. code-block:: console

   I: verify OK for 'hello' v1 (manifest authentic, module bound)
   I: Loaded extension hello
   app_edk: module 'hello' v1 signed by key 1 verified
   I: [app 0] hello: built outside the tree, run 1
   I: [app 0] hello: built outside the tree, run 2
   I: [app 0] hello: built outside the tree, run 3
   app_edk: module ended
