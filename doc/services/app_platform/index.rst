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

   getting_started
   services
   targets
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
* ``-EACCES`` - it holds the capability, but this GPIO line, LED pin, ADC, PWM
  or DAC channel, I2C address, SPI controller or chip select, counter,
  regulator or stepper instance, RGB LED pixel, display viewport, flash or
  EEPROM window or BLE GATT service was not assigned to it. The grants are made
  with :c:func:`app_platform_grant_gpio`, :c:func:`app_platform_grant_led`,
  :c:func:`app_platform_grant_adc`, :c:func:`app_platform_grant_pwm`,
  :c:func:`app_platform_grant_dac`, :c:func:`app_platform_grant_i2c`,
  :c:func:`app_platform_grant_spi`, :c:func:`app_platform_grant_spi_cs`,
  :c:func:`app_platform_grant_counter`, :c:func:`app_platform_grant_regulator`,
  :c:func:`app_platform_grant_stepper`, :c:func:`app_platform_grant_rgb`,
  :c:func:`app_platform_grant_display`, :c:func:`app_platform_grant_flash`,
  :c:func:`app_platform_grant_eeprom` and
  :c:func:`app_platform_grant_ble_gatt`, see
  :ref:`app_platform_services_catalog`.

Checking the capability first means an application that holds none cannot tell
an ungranted resource from one that does not exist.

The capabilities an application holds depend on how it is launched:

* A built-in application holds :c:member:`app_def.caps`.
* A module loaded with a signed manifest holds the capabilities the manifest
  requests within both the ceiling the launcher passes to
  :c:func:`app_platform_launch_llext` and the ceiling of the key that signed
  the manifest, see `Signing keys`_::

     held = requested & launcher ceiling & key ceiling

  A requested capability outside either ceiling is dropped with a log line
  naming its bits and the ceiling. When a capability the manifest requires is
  outside either ceiling, the launch is refused with ``-EPERM`` and a log line
  naming the missing bits and the ceiling that cut them.
* A module loaded without a manifest, by :c:func:`app_loader_verify_load`,
  holds the whole launcher ceiling. This path trusts the firmware that supplied
  the expected hash, and carries no request of its own.

:c:func:`app_platform_status` reports the capabilities an application holds.

The devices that applications reach by index are the ones listed in the
``zephyr,app-platform`` devicetree node, in index order: the first entry of a
list is instance 0 of that service. A device the node does not list is never
reachable by an application, whatever else the board describes. The node takes
these properties, see :zephyr_file:`dts/bindings/misc/zephyr,app-platform.yaml`:

* ``gpios`` - the GPIO lines applications may drive or watch, and
  ``line-names`` - their names, in the order of ``gpios``, which
  :c:func:`app_svc_gpio_find` looks up.
* ``counters`` - the counter devices of the hardware counter service.
* ``dacs`` - the DAC devices of the analog output service.
* ``regulators`` - the regulator devices of the regulator service.
* ``steppers`` - the stepper motor controllers of the stepper service.
* ``sensors`` - the sensor devices behind the sensor catalog. The first listed
  device that reports a kind's channel backs that kind.

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
opt into monitoring also get a watchdog channel, see `Liveness and the
watchdog`_.

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
fails until the next boot, with ``-EBUSY`` from most services and ``-EIO`` from the file
service. An application that dies while it only waits for the lock
does not disable the service. The external RAM and ring services still take the region of each
application reclaimed afterwards out of its domain, and leave that memory allocated. The other
services keep the resources they held for the applications reclaimed after the lock broke. Other
services and applications keep running.

.. _app_platform_liveness:

Liveness and the watchdog
=========================

The platform tells a dead application from a stuck one. Death needs no help
from the application: every slot has a death watcher, a small thread of
:kconfig:option:`CONFIG_APP_PLATFORM_WATCHER_STACK_SIZE` bytes that waits for the
application's entry thread to end. A fault reaches the supervisor first through
the fatal error handler, and a return from the entry point is seen by the
watcher. Either way the application is reclaimed. A return is an exit, so the
application is not relaunched; a fault marks it faulted.

A hang, an application that is alive but no longer makes progress, needs an
active check, so hang detection is opt-in. An application launched with
:c:member:`app_def.monitored` set gets a channel of the
:ref:`task watchdog <task_wdt_api>` with a timeout of
:kconfig:option:`CONFIG_APP_PLATFORM_WDT_TIMEOUT_MS` when it starts, and must
call :c:func:`app_svc_heartbeat` more often than that. The call feeds the
channel of the calling application through :c:func:`app_platform_heartbeat`,
which supervisor-mode code running on an application thread may call directly. The heartbeat needs
no capability, and from an application that is not monitored it does nothing,
so ordinary applications carry no heartbeat burden.

When a monitored application misses its deadline, the watchdog callback logs
that the application is not responding, marks it faulted and queues it for
reclaim. The supervisor then reclaims it like a crashed application: its
threads end, its watchdog channel is deleted, the reaper releases its
resources, and a resilient built-in application, see
:c:func:`app_platform_set_restart`, is relaunched with a fresh channel while it
stays within :kconfig:option:`CONFIG_APP_PLATFORM_MAX_RESTARTS`.
:c:member:`app_platform_status.faulted` and
:c:member:`app_platform_status.restarts` report what happened. The platform
initializes the task watchdog without a hardware fallback channel, and each
monitored application takes one of the
:kconfig:option:`CONFIG_TASK_WDT_CHANNELS` channels while it runs. The
:zephyr_file:`samples/subsys/app_liveness` sample runs a busy application, a
crashing one and a monitored one that stops feeding its watchdog.

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

CPU affinity
============

On an SMP kernel the platform can keep an application on a subset of the CPUs,
for example to give a time-critical application a CPU of its own. It needs
:kconfig:option:`CONFIG_APP_PLATFORM_CPU_AFFINITY`, which depends on
:kconfig:option:`CONFIG_SMP` and selects :kconfig:option:`CONFIG_SCHED_CPU_MASK`.
The mask is :c:member:`app_def.cpu_mask` for a built-in application and
:c:member:`app_launch_opts.cpu_mask`, passed to
:c:func:`app_platform_launch_llext_opts`, for a loaded one. Bit ``n`` stands for
CPU ``n``, and 0 lets the application run on any CPU.

The launch checks the mask before it takes a slot. A mask that names a CPU the
system does not have is refused with ``-EINVAL``, and so is a mask with more
than one bit when :kconfig:option:`CONFIG_SCHED_CPU_MASK_PIN_ONLY` is enabled. A
non-zero mask is refused with ``-ENOTSUP`` when the option is disabled. Without
:kconfig:option:`CONFIG_SMP` the mask is ignored.

The mask is applied to each application thread after it is created and before
it starts. A mask with one bit pins the thread to that CPU with
:c:func:`k_thread_cpu_pin`; a mask with several bits clears the thread's CPU
mask and enables each named CPU. The entry thread gets it at every start, and
every worker started with :c:func:`app_svc_thread_spawn` gets the same mask,
so all threads of an application stay on its CPUs. The mask is part of the
slot, so it is kept across :c:func:`app_platform_stop` and
:c:func:`app_platform_start` and when the supervisor relaunches the application
after a fault. The platform's own threads, the supervisor, the reaper and the
death watchers, are not restricted.

.. code-block:: c

   const struct app_def def = {
           .name = "control",
           .source = APP_BUILTIN,
           .entry = control_main,
           .caps = APP_CAP_LOG | APP_CAP_TIME,
           .cpu_mask = BIT(1),
   };

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

Waiting on several sources
==========================

``app_svc_event_wait()`` delivers input events, button presses, GPIO edges, BLE events and the stop
request, to a queue owned by one application. ``app_svc_event_subscribe()`` adds notifications from
the other sources: a timer slot expiring, an IPC message, a CAN frame, bytes on the secondary UART,
an MQTT message or socket data, a counter alarm, and an 802.15.4 datagram. A notification says data
is waiting; the application reads it with the source's own call and a zero timeout until that call
returns ``-EAGAIN``. One loop around ``app_svc_event_wait()`` can then serve every source without a
worker thread per source.

Timeouts of services added with the event queue are ``int32_t`` milliseconds where a negative
value waits forever. Older services take ``uint32_t`` milliseconds with no forever value and keep
that signature, as released entries never change; to wait on them forever, subscribe to their
notification and wait on the event queue instead.

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
checked with the key the manifest names from the key table compiled into the
firmware, see `Signing keys`_, and the module is accepted only if its SHA-256
matches the hash inside that authenticated manifest. A valid signature therefore
cannot be moved onto different bytes or relabelled, and the registry refuses to
install a version older than the one already recorded.

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

An installed image signed with a key that is now revoked or no longer listed
does not count as one this firmware cannot load: it no longer runs, but its
version still bounds what may replace it, so revoking a key cannot be used to
replay an older image signed with another key. The publisher signs a new
version, at least the installed one, with a valid key; uninstalling the
application, a local operator action, clears the bound.

:c:func:`app_registry_autostart_all` logs an error naming each application whose
launch at boot is refused and the reason, such as a required capability outside
the ceiling (``-EPERM``), another interface major version or target
(``-ENOEXEC``), a retired manifest format that needs the application to be
installed again (``-ENOTSUP``), a signing key this firmware does not trust
(``-ENXIO``) or a revoked one (``-ECANCELED``). The result is kept in RAM, not in the registry
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
     - signing key in the key table, 0 for the development key
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
see `Capabilities and grants`_. A manifest of a retired format, magic ``AMF1``
to ``AMF3``, is refused with ``-ENOTSUP`` and a log line that names the format
and asks for the application to be installed again.

Signing keys
============

The keys trusted to sign manifests are compiled into the firmware. Each has a
32-bit key id, which a manifest names in ``key_id``, a P-256 public key, a
capability ceiling, a prefix that the names of the applications it signs must
start with, and a revocation state. The loader checks the signature with the
key the manifest names and refuses:

* a key id the table does not hold with ``-ENXIO``,
* a revoked key with ``-ECANCELED``,
* a signature the named key did not make with ``-EPERM``,
* an application name that does not start with the prefix of the key with
  ``-EPERM``.

:c:func:`app_loader_key` returns the id and the ceiling of the key that signed a
loaded module.

The publisher keys are listed in the YAML file named by
:kconfig:option:`CONFIG_APP_PLATFORM_KEYS_FILE`, relative to the application
source directory, which the build reads at configure time:

.. code-block:: yaml

   keys:
     - id: 1                    # non-zero, unique
       key: keys/acme.pub.pem   # PEM public key, or the hex SEC1 point
       caps: [LOG, TIME, NET]   # capability ceiling: names, a number or ALL
       prefix: acme_            # names this key may sign, "" for any
     - id: 2
       revoked: true            # signs nothing; the key may be left out

A key path is relative to the list. A PEM file may hold the public key or, on a
development machine, the private key, whose public part is taken. The build
fails on a duplicate key id, a public key listed twice, a key that is not a
P-256 key, an unknown capability name or field, and on key id 0. An entry may
set ``generate: true`` to create a missing private key file; it is meant for
test lists, never for a product.

Key id 0 is the development key, trusted with every capability and any name
only while :kconfig:option:`CONFIG_APP_PLATFORM_DEV_KEY` is enabled, and the
firmware logs a warning at boot when it is. Its private key,
:kconfig:option:`CONFIG_APP_PLATFORM_DEV_KEY_FILE`, by default
``keys/app_signing_key.pem`` under the application source directory, is
generated unprotected on the first build and never committed, so it proves
nothing about who built a module. The samples and tests enable it; a product
disables it and lists its publisher keys.

To revoke a key, mark its entry ``revoked: true`` and keep the entry, so its key
id is never handed to another key, then update the firmware. Applications
signed with it are then refused at load and at boot, see
`Loading and verification`_.

The signing script ``sign_module.cmake`` signs with the development key under
``KEY_DIR`` unless it is given a private key kept outside the tree, as
``APP_SIGNING_KEY`` and ``APP_SIGNING_KEY_ID``. Given to the firmware build, on
the command line or in the environment, the two are recorded for every module
the build signs:

.. code-block:: console

   west build -b <board> <app> -- -DCONFIG_APP_PLATFORM_DEV_KEY=n \
           '-DCONFIG_APP_PLATFORM_KEYS_FILE="/secure/keys.yaml"' \
           -DAPP_SIGNING_KEY=/secure/acme.pem -DAPP_SIGNING_KEY_ID=1

The firmware build writes its key table to ``build/zephyr/app_platform_keys.json``
and the signer refuses to sign with a key id the table does not hold, a revoked
key, the development key when it is disabled, a private key that does not match
the public key listed under its id, a name outside the prefix of the key and a
required capability outside its ceiling, and warns about requested capabilities
outside the ceiling, which are not granted.

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

``west app info -d build`` prints the signed manifest of the bundle without a
device: the name, the version, the id of the key that signed it and the
capabilities it requests and requires.

Building applications outside the tree
======================================

A module writer does not need the Zephyr tree or the firmware sources. With
:kconfig:option:`CONFIG_APP_PLATFORM_EDK`, the :ref:`LLEXT EDK <llext_build_edk>`
of the firmware, built with ``west build -t llext-edk`` into
``build/zephyr/app-platform-edk.tar.xz``, is the module development kit: the
platform selects :kconfig:option:`CONFIG_LLEXT_EDK_HEADERS_SELECTED` and adds
its own files to the EDK. The kit holds:

* ``include/``: :zephyr_file:`include/zephyr/app_platform/app_sdk.h`, the
  generated declarations of the exported functions and the headers they include,
  and, in place of the firmware configuration header, a header defining the few
  architecture options those headers test. The build also preprocesses the SDK
  header with the flags of a module built in the firmware tree and fails when it
  includes a kernel, syscall, architecture, driver or devicetree header, so
  ``kernel.h`` is not part of the kit. Neither is the firmware configuration
  header.
* ``cmake.cflags`` and ``Makefile.cflags``: the compiler flags of the target,
  those of the LLEXT EDK.
* ``cmake/``: the toolchain of the target, the definitions of the C library and
  of the platform a module is compiled with in place of those of the firmware
  build, the module link flags, a toolchain file, and ``app_edk.cmake``, the
  helper that builds, checks and signs a module, which ``cmake.cflags`` also
  lists in ``LLEXT_CMAKE_MODULES``.
* ``firmware.txt``: what the signer binds a module to, the interface major
  version and the target fingerprint, with the board, the architecture and the
  toolchain the firmware was built with: its variant, target triple, compiler
  version and Zephyr SDK version.
* ``keys.json``: the public part of the key table, the key ids, their states,
  ceilings and name prefixes. ``abi/``: the lock file and the symbols a module may
  import.
* ``scripts/``: the signer and its Python requirements, Python 3.9 or later
  with the cryptography and PyYAML packages, and a ``README.txt``.

The kit does not hold the toolchain. The writer gives the path prefix of the
toolchain tools as ``APP_EDK_CROSS_COMPILE``, or the Zephyr SDK as
``ZEPHYR_SDK_INSTALL_DIR``, and the helper warns when the compiler version
differs from the one the kit names. A module project includes the helper before
``project()``:

.. code-block:: cmake

   cmake_minimum_required(VERSION 3.20.0)
   include(${APP_EDK_DIR}/cmake/app_edk.cmake)
   project(hello LANGUAGES C)
   app_edk_add_module(hello NAME hello VERSION 1 SOURCES hello.c
                      REQUESTED_CAPS LOG,TIME)

.. code-block:: console

   cmake -S hello -B build -G Ninja -DAPP_EDK_DIR=<kit> \
         -DAPP_EDK_CROSS_COMPILE=<sdk>/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi- \
         -DAPP_SIGNING_KEY=<publisher.pem> -DAPP_SIGNING_KEY_ID=1
   cmake --build build

The build writes the bundle ``hello.llext`` and ``hello.manifest``, which
``west app install`` and the loader take. A source including ``kernel.h`` does
not compile, a module importing a symbol outside the list is not signed, and
the signer refuses a key the firmware would refuse, as the firmware build does,
see `Signing keys`_. The development key, key id 0, signs only for a firmware
built with :kconfig:option:`CONFIG_APP_PLATFORM_DEV_KEY` and only for a writer
holding its private key.

A module built with the kit loads on the firmware the kit was made from and on
any later build of it with the same interface major version and target
fingerprint that still trusts the signing key. Other configuration options,
drivers, syscalls, tick rates and services do not change the modules the kit
builds: the kits of two such builds build the same module, byte for byte. A
module that calls a service a build leaves out fails to load on the unresolved
symbol. A build for another SoC or with other code generation options has another
fingerprint, a change of the interface that breaks modules raises the major
version, and a revoked or removed key refuses its modules; each needs a new kit
or another key.

The default LLEXT EDK ships every include directory of the firmware, its
configuration and its syscall headers, for extensions that call the kernel. A
module of the application platform must not, so its kit holds only the module
headers, and adds the signer and the firmware facts. A module built with the kit
is the same, byte for byte, as one built in the firmware tree from the same
source with ``app_platform_add_module()``.

:zephyr_file:`tests/subsys/app_platform_edk` builds two firmwares for one target,
with other syscall numbers and tick rates, builds a module with the kit of each
outside the tree and runs both modules in one firmware. The
:zephyr:code-sample:`app_edk` sample shows the firmware side and a module
project.

Configuration
*************

The platform is enabled with :kconfig:option:`CONFIG_APP_PLATFORM` and its
services are opted into individually, so an image links only what it uses. The
platform requires :kconfig:option:`CONFIG_TASK_WDT`. It enables
:kconfig:option:`CONFIG_USERSPACE` by default where the architecture supports
it; see `Without user mode`_ for builds without it. A firmware that defines
system calls of its own, as every sample does, also sets
:kconfig:option:`CONFIG_APPLICATION_DEFINED_SYSCALL`.

Each service has its own option, listed with the service in
:ref:`app_platform_services_catalog`. The other options fall into these groups.

Slots and threads
=================

* :kconfig:option:`CONFIG_APP_PLATFORM_MAX_APPS` - concurrent application slots.
  Each reserves a thread stack and a death watcher at build time, and a memory
  domain with user mode.
* :kconfig:option:`CONFIG_APP_PLATFORM_APP_STACK_SIZE` - the stack of each
  application's entry thread.
* :kconfig:option:`CONFIG_APP_PLATFORM_APP_PRIORITY` - the preemptible priority
  application threads run at. Keep it below the supervisor.
* :kconfig:option:`CONFIG_APP_PLATFORM_APP_THREADS` and
  :kconfig:option:`CONFIG_APP_PLATFORM_APP_THREAD_STACK_SIZE` - worker threads
  per application and their stack, see `Worker threads`_.
* :kconfig:option:`CONFIG_APP_PLATFORM_APP_SEMS` - semaphores per application.
* :kconfig:option:`CONFIG_APP_PLATFORM_CPU_AFFINITY` - per-application CPU
  masks on SMP, see `CPU affinity`_.
* :kconfig:option:`CONFIG_APP_PLATFORM_APP_HEAP_POOL_SIZE` - internal RAM for
  per-application heaps, zero to leave the feature out, and
  :kconfig:option:`CONFIG_APP_PLATFORM_APP_HEAP_MAX_BYTES` - the largest heap
  one application may declare, see `Per-application heap`_.

Supervision
===========

* :kconfig:option:`CONFIG_APP_PLATFORM_MAX_RESTARTS` - relaunches of a
  resilient application after a fault, zero for no limit.
* :kconfig:option:`CONFIG_APP_PLATFORM_WDT_TIMEOUT_MS` - the liveness timeout of
  monitored applications, see `Liveness and the watchdog`_.
* :kconfig:option:`CONFIG_APP_PLATFORM_STOP_WAIT_MS` - how long a stopped
  application may stay inside a service to release its locks.
* :kconfig:option:`CONFIG_APP_PLATFORM_FATAL_HANDLER` - whether the platform
  defines :c:func:`k_sys_fatal_error_handler`.
* :kconfig:option:`CONFIG_APP_PLATFORM_RECLAIM_HOOKS` - how many services can
  register a reclaim hook with :c:func:`app_platform_on_reclaim`.
* :kconfig:option:`CONFIG_APP_PLATFORM_SUPERVISOR_PRIORITY`,
  :kconfig:option:`CONFIG_APP_PLATFORM_SUPERVISOR_STACK_SIZE`,
  :kconfig:option:`CONFIG_APP_PLATFORM_WATCHER_STACK_SIZE`,
  :kconfig:option:`CONFIG_APP_PLATFORM_REAPER_PRIORITY` and
  :kconfig:option:`CONFIG_APP_PLATFORM_REAPER_STACK_SIZE` - the platform's own
  threads. The supervisor and the death watchers run above every application;
  the reaper runs below the supervisor.

Loader and registry
===================

* :kconfig:option:`CONFIG_APP_PLATFORM_LOADER` - load and verify signed llext
  modules. It needs :kconfig:option:`CONFIG_LLEXT` and
  :kconfig:option:`CONFIG_PSA_CRYPTO`.
* :kconfig:option:`CONFIG_APP_PLATFORM_LOADER_FILE_IN_RAM` - verify and link a
  module stored in a file from one private copy in RAM, rather than hashing the
  file as a stream and linking from it.
* :kconfig:option:`CONFIG_APP_PLATFORM_LOADER_MAX_MODULE_BYTES` - the llext
  memory one module may take; a larger module is refused.
* :kconfig:option:`CONFIG_APP_PLATFORM_KEYS_FILE` - the publisher keys trusted
  to sign modules, and :kconfig:option:`CONFIG_APP_PLATFORM_DEV_KEY` with
  :kconfig:option:`CONFIG_APP_PLATFORM_DEV_KEY_FILE` - whether the development
  key is trusted too, see `Signing keys`_.
* :kconfig:option:`CONFIG_APP_PLATFORM_EDK` - the development kit for modules
  built outside the tree, see `Building applications outside the tree`_.
* :kconfig:option:`CONFIG_LLEXT_EXPORT_SYMBOL_GROUP_APP_ABI` - export the module
  interface functions, see `Module interface`_.
* :kconfig:option:`CONFIG_APP_PLATFORM_REGISTRY` - persist which applications
  are installed, and which start at boot, with
  :kconfig:option:`CONFIG_APP_PLATFORM_REGISTRY_MAX_APPS`,
  :kconfig:option:`CONFIG_APP_PLATFORM_REGISTRY_NAME_MAX` and
  :kconfig:option:`CONFIG_APP_PLATFORM_REGISTRY_MOUNT_POINT`.

Service limits
==============

The services size their buffers and tables at build time. The main limits are:

* :kconfig:option:`CONFIG_APP_PLATFORM_BUS_XFER_MAX` - the largest I2C, SPI or
  UART transfer in one call, and
  :kconfig:option:`CONFIG_APP_PLATFORM_UART_RX_BUF` - the secondary UART receive
  buffer.
* :kconfig:option:`CONFIG_APP_PLATFORM_I2C_ADDRS` - I2C addresses per
  application, and :kconfig:option:`CONFIG_APP_PLATFORM_I2C_ERROR_LIMIT` -
  failed transfers in a row before an application loses its addresses.
* :kconfig:option:`CONFIG_APP_PLATFORM_CAN_FILTERS` and
  :kconfig:option:`CONFIG_APP_PLATFORM_CAN_RX_DEPTH` - CAN filters and queued
  frames per application.
* :kconfig:option:`CONFIG_APP_PLATFORM_EVENT_QUEUE_LEN` - events buffered per
  application.
* :kconfig:option:`CONFIG_APP_PLATFORM_FS_OPEN_FILES`,
  :kconfig:option:`CONFIG_APP_PLATFORM_FS_PATH_MAX` and
  :kconfig:option:`CONFIG_APP_PLATFORM_FS_XFER_MAX` - the file service's shared
  open-file table, path length and transfer size.
* :kconfig:option:`CONFIG_APP_PLATFORM_EEPROM_XFER_MAX` - the largest EEPROM
  read or write.
* :kconfig:option:`CONFIG_APP_PLATFORM_DISPLAY_XFER_MAX` - the display staging
  buffer of each application.
* :kconfig:option:`CONFIG_APP_PLATFORM_SENSOR_READ_N_MAX` - samples in one
  multi-sample sensor read.
* :kconfig:option:`CONFIG_APP_PLATFORM_RING_MAX_BYTES` and
  :kconfig:option:`CONFIG_APP_PLATFORM_PSRAM_MAX_BYTES` - the largest ring and
  external RAM region one application may map.
* :kconfig:option:`CONFIG_APP_PLATFORM_NET_DGRAM_MAX` - the largest socket
  datagram.
* :kconfig:option:`CONFIG_APP_PLATFORM_BLE_GATT_MAX_SERVICES` - GATT service
  UUIDs granted per application.

API status
**********

This subsystem is experimental. Its API may change without the deprecation
period a stable API would get.
