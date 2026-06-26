.. zephyr:code-sample:: app-more-services8
   :name: App services 8 - pulse counter

   Expose a hardware pulse counter to an isolated user-mode app through
   service syscalls.

Overview
********

The platform owns the PCNT unit and an LEDC PWM stimulus output. The app, from
user mode, asks the platform to emit a burst of pulses and reads back the
accumulated edge count, never touching the counter or the output directly.

Wiring
******

The pulse counter only counts real signal edges on its input pin. On the
ESP32-C6 devkit the stimulus (LEDC output, GPIO2) and the counter signal input
(GPIO3) are separate pads: connect a jumper wire from GPIO2 to GPIO3 so the
emitted PWM burst reaches the counter. Without the jumper the read path still
works but the counter reads zero.
