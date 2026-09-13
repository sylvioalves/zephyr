.. zephyr:code-sample:: app-uart-line

   :name: Isolated UART line-parameter application

   Set the secondary UART's framing from a user-mode application.

Overview
********

Launches an application that sets the secondary UART's baud rate, parity, stop
bits and data bits, writes bytes and reads them back. The board overlay routes
the transmit and the receive signal of the same UART to one pin, so the pad
carries the output straight back into the input and the application can check
what it sent without any wiring. Sending again after a reconfigure is what
shows the port was really reframed rather than the call merely accepted.

The platform owns the port throughout. The application never touches the
driver, unsupported combinations are refused at the service boundary, and the
configuration found at boot is restored when the application closes the port or
is reclaimed, so one application cannot leave it misconfigured for the next.

Requirements
************

A board whose devicetree chooses ``app-uart``, and userspace support. The
supplied overlay does this for ``esp32c5_devkitc`` and loops the port back
through the pin matrix; on other boards, wire the port's transmit pin to its
receive pin instead.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_uart_line
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

Each check prints ``PASS`` or ``FAIL`` with the value it observed. Without a
chosen ``app-uart`` the application reports that there is nothing to do and
exits.

See :ref:`app_platform_policy` for why a bus is exposed scoped rather than
whole.
