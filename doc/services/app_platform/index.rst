.. _app_platform:

Application Platform
####################

The application platform runs applications as isolated, supervised groups. Each
application is a user-mode thread in its own memory domain, reaches the system
only through capability-gated services, and is reclaimed if it faults or hangs
without disturbing the platform or the other applications.

An application is either built into the image or loaded at runtime as a
:ref:`linkable loadable extension <llext>`. A loaded one is accepted only after
its signed manifest is authenticated and the module bytes are matched against
the hash that manifest carries, so the name and version an application claims
are verified rather than asserted.

.. toctree::
   :maxdepth: 1

   policy
   api

Concepts
********

Groups and isolation
====================

Every application occupies a slot with its own memory domain and thread stack.
The identity behind a service call is the calling thread, which the kernel owns,
so an application cannot present itself as another one. When a slot is reused,
the per-resource grants of the previous tenant are dropped before the new
application starts.

Without user mode
=================

The platform also builds without :kconfig:option:`CONFIG_USERSPACE`, for targets
that have no memory protection hardware or cannot spare its cost. Applications
then run as supervisor threads without memory domains, and a loaded module calls
the services directly instead of through system calls. Signed loading,
supervision, the restart budget, reclaim and the capability and grant checks all
work as before, but only as a contract: nothing stops an application from
writing any memory or calling kernel functions directly, and a fault the
hardware does not report goes unnoticed. Run only trusted applications this way.
A module built for one mode is refused by a firmware of the other, because the
firmware ABI identifier covers the mode.

Capabilities and grants
=======================

Authority is split in two. A *capability* says an application may use a class of
service at all; a *grant* says which instance of a resource it may reach. The
two produce different errors, and the distinction is deliberate:

* ``-EPERM`` - the application does not hold the capability.
* ``-EACCES`` - it holds the capability, but this GPIO line, ADC or PWM channel,
  I2C address, SPI bus, LED pin, RGB LED pixel, display viewport or flash window
  was not assigned to it.

Checking the capability first means an application that holds none cannot tell
an ungranted resource from one that does not exist.

The GPIO lines, counters, DACs, regulators and steppers that applications reach
by index are the ones listed in the ``zephyr,app-platform`` devicetree node, in
index order. A device the node does not list is never reachable by an
application, whatever else the board describes.

.. code-block:: devicetree

   app-platform {
           compatible = "zephyr,app-platform";
           gpios = <&gpio0 2 GPIO_ACTIVE_HIGH>, <&gpio0 3 GPIO_ACTIVE_HIGH>;
           line-names = "status", "relay";
           counters = <&timer0 &timer1>;
   };

Supervision
===========

A supervisor launches applications and acts on faults. An application marked
resilient is relaunched after a fault, up to
:kconfig:option:`CONFIG_APP_PLATFORM_MAX_RESTARTS` times; one that keeps
faulting is left stopped rather than respawning without end. Applications that
opt into monitoring also get a watchdog channel.

Faults reach the supervisor through :c:func:`k_sys_fatal_error_handler`, which
the platform defines. An application that needs its own handler disables
:kconfig:option:`CONFIG_APP_PLATFORM_FATAL_HANDLER` and calls
:c:func:`app_platform_handle_fatal_error` first, returning when it reports the
fault as taken over.

When an application stops or is reclaimed, a reaper drops its resource grants
and returns the hardware it drove to idle: regulator enables it holds are
released, counters and steppers it started are stopped, PWM, motor, DAC, GPIO
and LED outputs it last set are driven back to zero, RGB LED pixels it last lit
are turned off, and a display viewport it drew in is filled with
:kconfig:option:`CONFIG_APP_PLATFORM_DISPLAY_CLEAR_COLOR`. An output another
application set more recently is left as it is. Other device state an application changed, such as the real-time clock
or EEPROM contents, is kept.

An application that dies inside a service while it holds the service's lock, through a fault in
the service or a stop that outlasts :kconfig:option:`CONFIG_APP_PLATFORM_STOP_WAIT_MS`, can leave
the state behind that lock half updated. The service is then disabled: every later call to it
fails with ``-EIO`` until the next boot, and the resources it held for the dead application are
not released. Other services and applications keep running.

Loading and verification
========================

Verification is two steps. The ECDSA P-256 signature authenticates the manifest,
and the module is accepted only if its SHA-256 matches the hash inside that
authenticated manifest. A valid signature therefore cannot be moved onto
different bytes or relabelled, and the registry refuses to install a version
older than the one already recorded.

The manifest also names the ABI the module was built for. The build derives an
ABI identifier from the syscall numbering and the application interface
headers, the signer writes it into the manifest, and the loader rejects a
module whose identifier differs from the running firmware's with ``-ENOEXEC``.
A module built for another firmware configuration therefore fails to load
instead of invoking the wrong system calls.

Configuration
*************

The platform is enabled with :kconfig:option:`CONFIG_APP_PLATFORM` and its
services are opted into individually, so an image links only what it uses. The
main options are:

* :kconfig:option:`CONFIG_APP_PLATFORM_MAX_APPS` - concurrent application slots.
  Each reserves a thread stack at build time.
* :kconfig:option:`CONFIG_APP_PLATFORM_LOADER` - load and verify signed llext
  modules.
* :kconfig:option:`CONFIG_APP_PLATFORM_REGISTRY` - persist which applications
  are installed, and which start at boot.
* :kconfig:option:`CONFIG_APP_PLATFORM_SERVICES` - the shared, capability-gated
  service library.

The platform requires :kconfig:option:`CONFIG_TASK_WDT`. It enables
:kconfig:option:`CONFIG_USERSPACE` by default where the architecture supports it; see
`Without user mode`_ for builds without it.

API status
**********

This subsystem is experimental. Its API may change without the deprecation
period a stable API would get.
