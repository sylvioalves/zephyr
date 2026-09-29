.. zephyr:code-sample:: app-display
   :name: Isolated display applications

   Share one display between the supervisor and two loadable user-mode
   applications.

Overview
********

The supervisor draws a status bar across the top of the ``zephyr,display`` panel
with one tile per application, and grants each of two verified loadable
applications one half of the panel below it. Each application learns its
viewport size and the panel pixel format from the display service, then sweeps
a bar across its half above a gradient band. The applications draw through
system calls and never see the panel driver or each other's pixels.

Each step of the bar is drawn right after :c:func:`app_svc_display_vsync` reports a
panel refresh, so the small update stays ahead of the scanout and the bar moves
without tearing at the panel refresh rate. On a panel that reports no refresh,
such as the ``qemu_x86`` ramfb, the applications pace themselves with a sleep.
Each application shows its frame rate in a box in the top right corner of its
half.

After six seconds the supervisor stops the application on the right. Its tile
turns red and the platform fills its half with
:kconfig:option:`CONFIG_APP_PLATFORM_DISPLAY_CLEAR_COLOR`, while the application
on the left keeps drawing. Supervisor drawing shares the panel through
:c:func:`app_platform_display_lock`.

Requirements
************

A board with userspace support and a ``zephyr,display`` panel with at least one
byte per pixel. The sample runs on ``qemu_x86``, which opens a window for its
ramfb display, and on the ESP32-P4X Function EV Board DSI panel.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_display
   :host-os: unix
   :board: qemu_x86
   :goals: run
   :compact:

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_display
   :board: esp32p4x_function_ev_board/esp32p4/hpcore
   :goals: build flash
   :compact:

Sample Output
*************

.. code-block:: console

   *** app_display: a shared display through a platform service ***
   app_display: panel 1024x768, 4 byte(s) per pixel
   app_display: two apps drawing, stopping 'right' in 6000 ms
   I: [app 0] display: app 0 viewport 510x732 rc=0
   I: [app 1] display: app 1 viewport 510x732 rc=0
   I: [app 0] display: no refresh events, pacing by sleep
   I: [app 1] display: no refresh events, pacing by sleep
   I: [app 0] display: app 0 frame 100, 0 failed, 20 fps
   I: [app 1] display: app 1 frame 100, 0 failed, 20 fps
   app_display: stopped 'right' rc=0

   *** done: 'left' active=1 after 'right' was stopped ***

See :ref:`app_platform` for the capability model.
