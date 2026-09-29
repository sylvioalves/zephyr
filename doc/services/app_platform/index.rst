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

Worker threads
==============

An application runs on one thread unless the platform reserves workers for it.
:kconfig:option:`CONFIG_APP_PLATFORM_APP_THREADS` sets how many workers each application may run
next to the thread its entry point runs on, and
:kconfig:option:`CONFIG_APP_PLATFORM_APP_THREAD_STACK_SIZE` the stack of each. Both are reserved
per slot at build time. An application holding ``APP_CAP_THREAD`` starts a worker with
:c:func:`app_svc_thread_spawn` and waits for it with :c:func:`app_svc_thread_join`.

A worker belongs to its application like the entry thread does: it runs in the same memory domain,
calls services with the same capabilities and grants, and runs at the application's priority or
below. The application is one unit for supervision. A fault in any of its threads reclaims all of
them, a stop or removal ends them together, and when the entry thread returns the workers end with
it.

The threads of one application coordinate through semaphores the platform keeps for it:
:c:func:`app_svc_sem_init`, :c:func:`app_svc_sem_give` and :c:func:`app_svc_sem_take`, up to
:kconfig:option:`CONFIG_APP_PLATFORM_APP_SEMS` per application. The application names them by
handle and never holds the kernel object, so the calls behave the same with and without user
mode. They are released when the application ends.

Per-application heap
====================

An application may declare a heap: a loadable one in the ``heap_size`` field of its signed
manifest, which the signing script fills from ``APP_HEAP_SIZE``, and a built-in one in
:c:member:`app_def.heap_size`. Because the size is signed, whoever launches a module cannot change
the memory it gets. The platform carves the heap from a pool of internal RAM of
:kconfig:option:`CONFIG_APP_PLATFORM_APP_HEAP_POOL_SIZE` bytes, zeroes it, initializes a heap at
its start and, with user mode, maps it into the application's memory domain before the first
thread starts. The region is rounded up to what the memory protection hardware maps: a naturally
aligned power of two with an MPU or PMP, whole pages with an MMU. A launch fails when the heap is
larger than :kconfig:option:`CONFIG_APP_PLATFORM_APP_HEAP_MAX_BYTES` (``-E2BIG``), when the pool
has no room for it (``-ENOMEM``), or when the pool size is zero (``-ENOTSUP``). With user mode the
heap takes one memory partition of the application's domain, next to the partitions of a loaded
module.

The application allocates with :c:func:`app_malloc`, :c:func:`app_calloc`,
:c:func:`app_realloc`, :c:func:`app_aligned_alloc` and :c:func:`app_free` from
:zephyr_file:`include/zephyr/app_platform/app_heap.h`. They run the allocator in the calling
thread, so the heap metadata is only ever walked by the application that owns it, and a corrupted
heap faults that application alone. The threads of one application share the heap through a lock
word in the region. :c:func:`app_svc_heap_info` returns the region's address and size.

A stopped application keeps its region, which is zeroed and initialized again when it starts. A
removed application's region is unmapped, zeroed and returned to the pool by the reaper, without
taking any service lock, so the next application in the slot neither sees the old contents nor
can reach the old region.

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

Installing from the host
========================

``west app install`` installs a signed module on a running device over its serial shell. It
finds the bundle a build produced, a ``<name>.llext`` module next to the ``<name>.manifest`` file
that holds the signed manifest and its signature, uploads both into the device storage with the
MCUmgr file system group carried by the shell, and runs the device's install command through the
MCUmgr shell management group. The device performs the verification described above and the
command exits non-zero with the device's error when it refuses the module. See
:ref:`app_install_sample` for a firmware that accepts it:

.. code-block:: console

   west app install -d build -p /dev/ttyUSB0

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
* :kconfig:option:`CONFIG_APP_PLATFORM_APP_HEAP_POOL_SIZE` - internal RAM for
  per-application heaps. Zero leaves the feature out.

The platform requires :kconfig:option:`CONFIG_TASK_WDT`. It enables
:kconfig:option:`CONFIG_USERSPACE` by default where the architecture supports it; see
`Without user mode`_ for builds without it.

API status
**********

This subsystem is experimental. Its API may change without the deprecation
period a stable API would get.
