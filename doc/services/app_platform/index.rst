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

A module does not depend on the mode: it reaches the services only through the
functions the firmware exports, see `Module interface`_, so the same signed
module runs in a firmware with user mode and in one without. Without user mode
the module executes from the llext heap as kernel code. The Arm MPU of an XIP
target and the RISC-V PMP with
:kconfig:option:`CONFIG_PMP_DATA_EXECUTION_PREVENTION` make RAM non-executable, so
the loader enables :kconfig:option:`CONFIG_LLEXT_HEAP_EXECUTABLE`, which maps the
llext heap executable for the kernel with a static MPU region or a locked PMP
entry. That heap then holds writable and executable memory at once. On an Armv7-M
MPU the heap size, :kconfig:option:`CONFIG_LLEXT_HEAP_SIZE`, must be a power of
two. With the option disabled, a RISC-V module whose code would land in
non-executable memory is refused with ``-ENOEXEC``, and on the Arm MPU it faults
on its first instruction and is reclaimed.

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

The capabilities an application holds depend on how it is launched:

* A built-in application holds :c:member:`app_def.caps`.
* A module loaded with a signed manifest holds the capabilities the manifest
  requests within the ceiling the launcher passes to
  :c:func:`app_platform_launch_llext`. A requested capability outside the
  ceiling is dropped with a log line naming its bits. When a capability the
  manifest requires is outside the ceiling, the launch is refused with
  ``-EPERM`` and a log line naming the missing bits.
* A module loaded without a manifest, by :c:func:`app_loader_verify_load` or
  :c:func:`app_loader_verify_load_signed`, holds the whole ceiling. These paths
  trust the firmware that supplied the expected hash or the key, and carry no
  request of their own.

:c:func:`app_platform_status` reports the capabilities an application holds.

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

Configuring an application before it starts
============================================

A launched application can run on another CPU, or preempt its launcher, before the launch call
returns, so a memory partition or a grant added after a plain launch can arrive after the
application first needs it. A launch with :c:member:`app_def.stopped` set, or with
:c:member:`app_launch_opts.stopped` for :c:func:`app_platform_launch_llext_opts`, returns the group
id of an application that is set up but not running: its memory domain already maps its module and
heap, and :c:func:`app_platform_status` reports it inactive. The launcher adds partitions to
:c:func:`app_platform_domain` and then starts the application with :c:func:`app_platform_start`.
Until the firmware starts it, the application is held: an application holding
``APP_CAP_CONTROL`` that starts or restarts it with :c:func:`app_svc_app_start` or
:c:func:`app_svc_app_restart` gets ``-EAGAIN``. :c:func:`app_platform_remove` releases an
application that never started like any other.

Grants are dropped whenever the application stops or is reclaimed, so they are set in a configure
hook, :c:member:`app_def.configure` or :c:member:`app_launch_opts.configure`. The platform calls it
with the group id and the configure user pointer every time the application is about to start: at
a launch that is not stopped, in :c:func:`app_platform_start` and when the supervisor relaunches
the application after a fault. It runs once the cleanup of the previous run is complete and before
any application code runs, so the grants are in place for every run. The hook runs on the thread
that starts the application, the supervisor for a relaunch, so it must not block and is limited to
setting grants and adding memory partitions. When it returns an error the application does not
start: a launch returns the error and leaves nothing behind, and a start or a relaunch leaves the
application stopped and held.

.. code-block:: c

   static int sensor_configure(int id, void *user)
   {
           ARG_UNUSED(user);
           app_platform_grant_gpio(id, BIT(0));
           return 0;
   }

   const struct app_def def = {
           .name = "sensor",
           .source = APP_BUILTIN,
           .entry = sensor_main,
           .caps = APP_CAP_LOG | APP_CAP_GPIO,
           .stopped = true,
           .configure = sensor_configure,
   };
   int id = app_platform_launch(&def);

   k_mem_domain_add_partition(app_platform_domain(id), &shared_part);
   app_platform_start(id);

A partition added to a domain stays while the application is stopped, started again or relaunched
after a fault, which reuses its slot. A new application launched into a slot gets the domain back
with none of the partitions its previous tenant had, so a launcher adds its partitions again at
every launch.

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

An application that dies inside a service while it owns the service's lock, through a fault in
the service or a stop that outlasts :kconfig:option:`CONFIG_APP_PLATFORM_STOP_WAIT_MS`, can leave
the state behind that lock half updated. The service is then disabled: every later call to it
fails with ``-EIO`` until the next boot. An application that dies while it only waits for the lock
does not disable the service. The external RAM and ring services still take the region of each
application reclaimed afterwards out of its domain, and leave that memory allocated. The other
services keep the resources they held for the applications reclaimed after the lock broke. Other
services and applications keep running.

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
:zephyr_file:`include/zephyr/app_platform/app_heap.h`, or a loadable module through the same
names in ``app_sdk.h``, see `Module interface`_. They run the allocator in the calling thread, so
the heap metadata is only ever walked by the application that owns it, and a corrupted heap faults
that application alone. The threads of one application share the heap through a lock
word in the region. :c:func:`app_svc_heap_info` returns the region's address and size.

A stopped application keeps its region, which is zeroed and initialized again when it starts. A
removed application's region is unmapped, zeroed and returned to the pool by the reaper, without
taking any service lock, so the next application in the slot neither sees the old contents nor
can reach the old region.

Networking
==========

With :kconfig:option:`CONFIG_APP_PLATFORM_NET` the platform owns the network link, the sockets
and the MQTT sessions, and each application reaches them through the ``app_svc_net_*``,
``app_svc_udp_*``, ``app_svc_tcp_*`` and ``app_svc_mqtt_*`` services. An application owns one
socket and one MQTT session, keyed by its slot, and both are closed when it is reclaimed.

The link is chosen with the ``APP_PLATFORM_NET_LINK`` choice. On a Wi-Fi link, the default when
:kconfig:option:`CONFIG_WIFI` is enabled, the platform associates the first Wi-Fi interface with
the configured network and ``app_svc_net_ready()`` returns once it holds an address and the broker
resolves. With :kconfig:option:`CONFIG_APP_PLATFORM_NET_LINK_IFACE` the platform configures no
interface: the link is up once an interface that is up holds an IPv4 address, the default
interface first, so Ethernet or loopback serve as well. The broker is then resolved when an
application opens its MQTT session, and the Wi-Fi connect and disconnect services return
``-ENOTSUP``.

A supervisor net thread runs every socket operation, so the network stack never runs on an
application's stack. It blocks in one ``zsock_poll()`` over an eventfd and the open sockets and
MQTT sessions. An application request writes the eventfd and arriving data makes its socket
readable, so either is served as soon as it is there rather than on a polling period, and a
receive call that waits for data returns as soon as it is queued. The poll timeout is only the
next MQTT keep-alive, or the Wi-Fi signal-strength refresh. A received datagram that finds the
application's inbox full is dropped, while a stream is not read until the application drains its
inbox, which leaves the flow control to TCP. The poll set needs
:kconfig:option:`CONFIG_ZVFS_POLL_MAX` of at least one more than twice
:kconfig:option:`CONFIG_APP_PLATFORM_MAX_APPS`, which the platform sets by default and checks at
build time.

Bulk calls
==========

Every service call crosses into the platform once, so work that repeats a call per item pays that
crossing per item. Two services take the items in one call:

* ``app_svc_gpio_set_mask()`` drives every line in a mask to the level given in a value, with
  each line checked against the application's grant before any line changes. The lines change
  under one hold of the service lock, so no other caller runs between them, and the lines of one
  GPIO port change in one driver call.
* ``app_svc_sensor_read_n()`` takes up to
  :kconfig:option:`CONFIG_APP_PLATFORM_SENSOR_READ_N_MAX` readings of a sensor kind under one hold
  of the device and writes them to the caller's buffer, which is checked once.

A byte stream already has a bulk path: the ring service (``app_svc_ring_bind()``) maps a buffer
into the application, which fills or drains it without a call and then moves the whole batch with
one ``app_svc_ring_flush()`` or ``app_svc_ring_fill()``.

Loading and verification
========================

Verification is two steps. The ECDSA P-256 signature authenticates the manifest,
and the module is accepted only if its SHA-256 matches the hash inside that
authenticated manifest. A valid signature therefore cannot be moved onto
different bytes or relabelled, and the registry refuses to install a version
older than the one already recorded.

That refusal has one exception, for a firmware rollback. After the firmware
reverts to an older interface major version, the installed version of an
application built for the newer one no longer loads, and the older version
built for this firmware is lower. :c:func:`app_registry_commit` therefore
installs a lower version, and logs it, when the installed manifest has a retired
format or names another interface major version or target, and the new manifest
names those of this firmware. A lower version is still refused over an image
this firmware loads, so an old signed image cannot be replayed over a working
one. :c:func:`app_registry_install_versioned` knows nothing of the new image and
makes no exception. When the firmware moves forward again, the newer version is
installed as usual.

:c:func:`app_registry_autostart_all` logs an error naming each application whose
launch at boot is refused and the reason, such as a required capability outside
the ceiling (``-EPERM``), another interface major version or target
(``-ENOEXEC``) or a retired manifest format that needs the application to be
installed again (``-ENOTSUP``). The result is kept in RAM, not in the registry
file, and :c:func:`app_registry_autostart_result` returns it, so the firmware can
report or act on a refused application.

The manifest also carries the major version of the module interface the module
was built against, from the ``major`` line of
:zephyr_file:`subsys/app_platform/abi/app_abi.txt`, and the loader refuses a
module of another major version with ``-ENOEXEC``.

A module runs only on the target it was built for. The firmware build describes
its target by the architecture, the SoC (:kconfig:option:`CONFIG_SOC`) and the
code generation options it passes the compiler, those starting with ``-m`` such
as ``-march``, ``-mabi``, ``-mcpu``, ``-mfpu`` and ``-mfloat-abi``, and hashes
that description to a 32-bit fingerprint. The signer writes the fingerprint
into the manifest and the loader refuses a module whose fingerprint differs
from the one compiled into the firmware with ``-ENOEXEC``. The fingerprint and
the description are written to ``build/zephyr/app_platform_abi.txt``. The
fingerprint leaves out the kernel configuration, such as user mode, the tick
rate, the syscall numbering and the services built in, none of which a module
depends on: a module that calls a service the firmware lacks fails to load on
the unresolved symbol instead. The loader
also checks the ELF flags of the module before hashing it and refuses a
mismatch with ``-ENOEXEC``: on RISC-V the floating-point ABI and the compressed
and embedded base ISA bits, on Arm the EABI version and a float ABI bit when the
object sets one. An Arm relocatable object keeps its floating-point calling
convention in build attributes rather than in its flags, which leaves it to the
fingerprint.

Manifest format
---------------

The signed manifest is a header of three fields followed by the fields of the
format, all little-endian and packed. The signature covers exactly ``hdr_len``
bytes. The signing script ``sign_module.cmake`` builds it from
``APP_NAME``, ``APP_VERSION``, ``APP_HEAP_SIZE``, ``APP_REQUESTED_CAPS`` and
``APP_REQUIRED_CAPS`` and from what the firmware build writes to
``build/zephyr/app_platform_abi.txt``. The capabilities are given as a number
or as names without the ``APP_CAP_`` prefix, such as ``LOG,TIME``.
``APP_REQUESTED_CAPS`` must be set, since the application is granted nothing it
does not request, and the signer refuses required capabilities that are not
requested.

.. list-table::
   :header-rows: 1

   * - Offset
     - Field
     - Meaning
   * - 0
     - ``uint32 magic``
     - ``AMF4``, ``0x34464d41``
   * - 4
     - ``uint16 hdr_len``
     - length of the signed manifest, at least 160 and at most 256
   * - 6
     - ``uint16 format``
     - format of the manifest, 4 or later
   * - 8
     - ``uint32 version``
     - application version, never lower than the installed one
   * - 12
     - ``uint32 module_len``
     - module length in bytes
   * - 16
     - ``char name[32]``
     - application name, NUL-terminated
   * - 48
     - ``uint8 module_sha256[32]``
     - SHA-256 of the module
   * - 80
     - ``uint8 reserved1[16]``
     - zero
   * - 96
     - ``uint32 target``
     - fingerprint of the target the module was built for, must equal the
       firmware's
   * - 100
     - ``uint16 abi_major``
     - interface major version, must equal the firmware's
   * - 102
     - ``uint16 reserved0``
     - zero
   * - 104
     - ``uint32 heap_size``
     - per-application heap in bytes, 0 for none
   * - 108
     - ``uint32 key_id``
     - signing key, 0 for the default key
   * - 112
     - ``uint64 requested_caps``
     - capabilities the application asks for
   * - 120
     - ``uint64 required_caps``
     - capabilities it cannot run without, a subset of the requested ones
   * - 128
     - ``uint32 flags``
     - zero, no flag is defined
   * - 132
     - ``uint8 reserved[28]``
     - zero

A firmware accepts any ``hdr_len`` from the length of the fields it knows up to
256 bytes and ignores the fields past them, so a later format can append fields
that an older firmware skips while still checking the signature over them. A
manifest shorter than the fields the firmware knows, or presented with another
length than its ``hdr_len``, is refused with ``-EBADMSG``, and so is one that
requires a capability it does not request. The loader records the capabilities
of a verified module, and the launch grants them within the launcher's ceiling,
see `Capabilities and grants`_. A manifest of the previous fixed-layout format,
magic ``AMF3``, is refused with ``-ENOTSUP`` and a log line asking for the
application to be installed again.

Module interface
================

A loadable module is built with ``app_platform_add_module()``, which takes the arguments of
``add_llext_target()``, and includes only :zephyr_file:`include/zephyr/app_platform/app_sdk.h`. The
header gives the module the services under their ``app_svc_*`` names, the kernel services
``app_sleep_ms()``, ``app_sleep_us()``, ``app_uptime_get()`` and ``app_yield()``, which take
explicit units rather than ticks, the heap allocator, the shared service types and the
``app_main()`` entry point. Each of these is an inline call of a function the firmware exports
under a versioned name, ``app_abi_<name>_v<version>``, which runs the service in the firmware with
all of its checks. A module therefore holds no syscall number, kernel structure or kernel
configuration value of the firmware it was built against.

The exported functions are listed once, in :zephyr_file:`subsys/app_platform/abi/app_abi.txt`.
The build generates the firmware functions, their declarations and the module names from it, and
checks every service entry against the declaration in ``app_services.h``. Only the functions of
the services built into the firmware are exported, in the ``APP_ABI`` symbol group enabled by
:kconfig:option:`CONFIG_LLEXT_EXPORT_SYMBOL_GROUP_APP_ABI`, so a module that calls a missing
service fails to load on the unresolved symbol. A released entry never changes: a new signature or
behavior takes a new entry with the next version, and the old entry stays for the modules built
against it.

The released interface is recorded in :zephyr_file:`subsys/app_platform/abi/app_abi.lock`: every
exported function with its signature, the typedefs, constants, capability bits and enumerators of
``app_svc_types.h`` and ``app_caps.h``, the layout of every structure in ``app_svc_types.h``, the
``app_main()`` entry point and the major version. The generator computes these lines from the
sources on every build and fails when a recorded line changed or disappeared, pointing at the
line and the versioning rule, and when the interface has an entry the lock does not record yet,
listing the lines to append. ``west build -t app_platform_abi_lock``, or
``subsys/app_platform/abi/gen_app_abi.py --update-lock`` without a build, appends them; it never
changes a recorded line unless the major version was raised, when it rewrites the file. The
recorded layouts and values also become build assertions, so every target's compiler confirms
them. The lock does not depend on the configuration: a service built out of the firmware keeps
its entries. The rules are described in :ref:`app_platform_abi_stability`.

A module must not include ``kernel.h``, a header that declares syscalls, or ``app_services.h``.
``app_sdk.h`` refuses to build after one of them and, with GCC or Clang, makes one included after
it fail. ``app_platform_add_module()`` also defines ``APP_PLATFORM_MODULE``, which makes
``app_services.h`` refuse to build. Built-in applications are part of the firmware and keep
using ``app_services.h``.

A module may import only the exported functions and the symbols listed for its architecture in
:zephyr_file:`subsys/app_platform/abi/app_imports.txt`: pure C library functions, such as
``snprintf()`` and the string functions, and the compiler run-time helpers the compiler emits
calls to, such as the ``__aeabi_*`` division and memory helpers on Arm. The build generates the
list from both files. The signer runs ``nm -u`` on the module and refuses to sign one that imports
anything else, and the loader reads the symbol table of a verified module before linking it and
refuses it with ``-EACCES`` when it imports anything else, so a module cannot reach a kernel
function or the implementation of a service directly even though the firmware exports it for
other extensions. The check reads symbol names, so the verifying loader is not available with
:kconfig:option:`CONFIG_LLEXT_EXPORT_BUILTINS_BY_SLID`.

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
