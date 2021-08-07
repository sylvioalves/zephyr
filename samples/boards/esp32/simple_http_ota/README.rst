.. _simple_http_ota:

Simple HTTP Over-The-Air Update
###########################

Overview
********

This sample describes how to manually update device firmware over the air
using standard HTTP protocol.

The MCUboot bootloader is required for this sample to function properly.
More information about the Device Firmware Upgrade subsystem and MCUboot
can be found in :ref:`mcuboot`.

Building and Running
********************

The steps below describe how to build and run the sample code in a ESP32 board
using WiFi connection. Make sure you have the ESP32 board connected over USB port and
available WiFi network properly configured.

Step 1: Build and Sign Updatable Firmware
=========================================

For this sample, we will use `hello_world` sample as updatable image. After build
the sample, sign the image to allow mcuboot integration:

.. code-block:: console

   west build -b esp32 samples/hello_world
   west sign -t imgtool

`zephyr.signed.bin` will be generated at `build/zephyr` folder.
Rename this file to `hello.bin` and move it to a known folder to enable next steps.

Step 2: Start the Python Simple HTTP Server
===================================

To set up a HTTP download server, Python SimpleHTTPServer is used.
In the same folder that `hello.bin` was moved, run the command below to make the binary
available for download:

.. code-block:: console

   sudo python3 -m http.server 80

This will start the server on the host system. `hello.bin` file will become
downloadable with the URL ``<your-ip-address>/hello.bin``.

Step 2: Build, Sign and Flash Sample Code
=========================================

In `prj.conf` file, configure WiFi credentials and server URL:

.. code-block:: console

   # WiFI credentials
   CONFIG_ESP32_WIFI_SSID="ssid"
   CONFIG_ESP32_WIFI_PASSWORD="pass"

   # Python server file URL
   CONFIG_SIMPLE_HTTP_OTA_FILE_URL=<your-ip-address>/hello.bin

Next, build, sign and flash the sample code:

.. code-block:: console

   west build -b esp32 samples/boards/esp32/simple_http_ota
   west sign -t imgtool
   west flash

Sample Output
=============

To check output of this sample, any serial console program can be used (i.e. on Linux minicom, putty, screen, etc)
This example uses ``west espressif monitor``:

.. code-block:: console

   $ west espressif monitor

.. code-block:: console

   *** Booting Zephyr OS build zephyr-v3.0.0-3694-g306d1db489f9  ***
   [00:00:02.126,000] <inf> main: OTA sample app started
   [00:00:02.127,000] <inf> simple_http_ota: Image is not confirmed OK
   [00:00:02.127,000] <inf> simple_http_ota: Erasing bank in slot 1...
   [00:00:04.437,000] <inf> main: Waiting IP address from network..
   [00:00:04.440,000] <inf> esp32_wifi: WIFI_EVENT_STA_START
   [00:00:06.009,000] <inf> esp32_wifi: WIFI_EVENT_STA_CONNECTED
   [00:00:07.579,000] <inf> net_dhcpv4: Received: 192.168.68.147

Sample Console Interaction
==========================

After board gets proper IP address, the manual update procedure can be started by running
the shell command `ota run`:

.. code-block:: console

   uart:~$ ota run
   Starting SIMPLE HTTP OTA...
   [00:00:09.454,000] <inf> simple_http_ota: URL: <your-ip-address>/hello.bin
   [00:00:25.353,000] <inf> simple_http_ota: Update installed. Restart the board to complete.

Once file is downloaded and properly flashed into slot bank 1, reset the board so that MCUBoot
performs image check and image swapping:

.. code-block:: console

   uart:~$ kernel reboot cold

The board will restart and display the new content:

.. code-block:: console

   uart:~$ kernel reboot cold

   [esp32] [INF] *** Booting MCUboot build v1.7.0-485-ge86f575f ***
   [esp32] [INF] Primary image: magic=unset, swap_type=0x1, copy_done=0x3, image_ok=0x3
   [esp32] [INF] Scratch: magic=unset, swap_type=0x1, copy_done=0x3, image_ok=0x3
   [esp32] [INF] Boot source: primary slot
   [esp32] [INF] Swap type: test
   [esp32] [INF] Starting swap using scratch algorithm.
   [esp32] [INF] Disabling RNG early entropy source...
   [esp32] [INF] br_image_off = 0x10000
   [esp32] [INF] ih_hdr_size = 0x20
   [esp32] [INF] Loading image 0 - slot 0 from flash, area id: 1
   [esp32] [INF] DRAM segment: start=0xeea, size=0x35c, vaddr=0x3ffb0000
   [esp32] [INF] IRAM segment: start=0x1248, size=0x3188, vaddr=0x40080000
   [esp32] [INF] start=0x40082fe8

   *** Booting Zephyr OS build zephyr-v3.0.0-3694-g306d1db489f9  ***
   Hello World! esp32
