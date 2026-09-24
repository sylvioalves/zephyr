.. _app_fetch_sample:

App store over Wi-Fi
####################

Overview
********

The device pulls a signed application from a server over Wi-Fi, verifies it, and
launches it as an isolated user-mode app. At boot the device joins Wi-Fi and
waits for an address. The shell command::

   store fetch <name> <server-ip>

downloads two files over HTTP into storage::

   http://<server-ip>:8000/<name>.llext     -> /lfs/<name>.llext
   http://<server-ip>:8000/<name>.manifest  -> /lfs/<name>.manifest

then verifies the signed manifest against the public key compiled into the
firmware, binds the module, records it in the registry, and launches it -
streamed from flash. A tampered or unsigned download is rejected.

Configure Wi-Fi
***************

Set the network in ``prj.conf`` (or a local overlay) before building::

   CONFIG_APP_FETCH_WIFI_SSID="your-ssid"
   CONFIG_APP_FETCH_WIFI_PSK="your-password"

Build and run
*************

.. code-block:: console

   west build -b esp32c5_devkitc/esp32c5/hpcore samples/subsys/app_fetch
   west flash

On a board with external RAM (PSRAM), such as the ESP32-C5-DevKitC, the Wi-Fi
driver heap can be placed in PSRAM to free internal SRAM:

.. code-block:: console

   west build -b esp32c5_devkitc/esp32c5/hpcore samples/subsys/app_fetch -- \
      -DEXTRA_CONF_FILE=overlay-psram.conf

Host the app
************

The build writes the reference app to ``build/serve``: ``weather.llext`` and
``weather.manifest``, the 92-byte manifest followed by its 64-byte signature.
Serve that directory on port 8000 from a machine on the same network::

   python3 -m http.server 8000 --directory build/serve

A module signed outside this build must carry the device firmware's ABI
identifier, written to ``build/zephyr/app_platform_abi_id.txt``. The device
refuses a manifest with any other identifier.

On the device shell::

   uart:~$ store fetch weather 192.168.1.10
   fetching 'weather' from 192.168.1.10 ...
   downloaded; verifying signature ...
   fetched, verified and launched 'weather' v1 (isolated app 0)

A download whose bytes do not match the signed manifest hash is rejected and
never launched.
