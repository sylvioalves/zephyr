.. zephyr:code-sample:: app-rgb
   :name: Isolated RGB LED application

   Let a loadable user-mode application set one granted RGB LED pixel.

Overview
********

A verified loadable application is launched in user mode with the RGB LED
capability and a grant for pixel 0 of the board's addressable LED strip. It
cycles the pixel through red, green, blue and off, two seconds per step,
logging each step before the color is shown and the result of the service call
after it. The platform owns the strip and its driver; the application only
names a pixel and a color.

Requirements
************

A board with an addressable LED strip on the ``led-strip`` devicetree alias and
userspace support. The overlays for the ESP32-C5 and ESP32-C6 DevKitC drive the
on-board WS2812 LED through I2S.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_rgb
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

See :ref:`app_platform` for the capability model.
