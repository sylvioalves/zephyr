.. _app_platform_policy:

Capability Policy
#################

The platform is not all of Zephyr exposed to applications; it is a deliberately
classified subset. This page is the curation layer: which subsystems are
reachable from an application, in what shape, and which are reserved for the
system. Every subsystem sorts into one of seven shapes, so adding one is a
classification decision rather than a new mechanism.

The seven shapes
****************

.. list-table::
   :header-rows: 1
   :widths: 20 55 25

   * - Shape
     - Meaning
     - Reference
   * - Open
     - Read-only, shared; gated only by the service's capability.
     - sensor catalog
   * - Scoped
     - One named instance granted at launch, enforced before access.
     - scoped I2C
   * - Namespaced-scoped
     - A subtree or keyspace the application cannot escape.
     - filesystem sandbox
   * - Brokered connection
     - A stateful link the platform owns; events reach the application through
       its queue.
     - BLE, MQTT
   * - Arbitrated pool
     - A scarce fixed pool; the platform hands out instances and denies the
       one past the limit.
     - BLE connections
   * - Admin
     - Device-wide configuration. There is no separate admin capability: each
       such operation is gated by a capability of its own or by its service's.
     - setting the hardware clock
   * - Never exposed
     - Platform foundation, with no application surface at all.
     - by omission

What an application sees collapses these to three tiers: open, scoped (covering
scoped, namespaced-scoped, arbitrated pool and brokered connection) and admin. A
never-exposed subsystem simply has no service symbol.

Every service checks the calling application's capability mask first, so even
an open service is reachable only with its capability. The launcher sets that
mask when it starts the application and makes the per-instance grants
(``app_platform_grant_*``). The signed manifest carries the application's
identity (name, version, module hash and interface version) and the
capabilities it requests and requires. The loader records those, but the mask
the launcher sets still decides what the application may use.

.. _app_platform_abi_stability:

Interface stability
*******************

A module is built once and keeps running on later firmware for the same target.
What it depends on is the module interface: the functions listed in
:zephyr_file:`subsys/app_platform/abi/app_abi.txt`, the types, constants,
capability bits and enumerators of ``app_svc_types.h`` and ``app_caps.h``, and
the ``app_main()`` entry point. The released interface is recorded in
:zephyr_file:`subsys/app_platform/abi/app_abi.lock`, and every build holds the
sources to it:

* Entries are only appended. A new service, type, constant, capability bit or
  enumerator adds lines to the lock, which the build lists and
  ``gen_app_abi.py --update-lock`` appends. A recorded line never changes.
* A function whose signature or behavior changes takes a new entry with the
  next version. The old version stays exported, as an entry the firmware
  implements, for the modules built against it.
* A structure shared with modules never changes. A changed structure is a new
  structure, passed by a new version of the functions that use it. Shared
  structures use fixed-width fields and explicit padding, so their layout is the
  same on every target, and the build refuses one whose layout depends on the
  alignment of 64-bit fields.
* Anything else, such as removing an entry or renumbering a capability, breaks
  the modules already built and takes a new major version in ``app_abi.txt``.
  The lock is then rewritten, and a firmware loads only modules of its own
  major version.

The interface holds no syscall number, kernel structure or kernel configuration
value. A module built against one firmware therefore runs in another one with
other drivers, other syscalls, another tick rate, with or without user mode, as
long as both are built for the same target and export the functions the module
calls. The target is the architecture, the SoC and the code generation options
of the compiler, whose fingerprint the signed manifest carries.

Classifying a subsystem
***********************

For each subsystem, in order:

#. Does an application touching it endanger the system or the other
   applications, in a way scoping cannot contain? Then it is never exposed:
   memory protection, clocks, the bootloader.
#. Is it read-only shared state? Then it is open: reading a sensor, reading the
   time.
#. Does it configure the whole device? Then it is admin: setting the time,
   setting credentials.
#. Is it one resource an application should own a slice of? Then it is scoped:
   one I2C address, one UART, one counter.
#. Is it storage? Then it is namespaced-scoped: its own directory or key
   prefix.
#. Is it a stateful link delivering asynchronous events? Then it is a brokered
   connection.
#. Is it a scarce hardware pool? Then it is an arbitrated pool.

One peripheral often splits by operation, reading being open while configuring
is admin. The hardware real-time clock divides that way: reading it needs
``APP_CAP_RTC`` and setting it needs ``APP_CAP_RTC_SET``.

Connectivity
************

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - Subsystem
     - Tier
     - Notes
   * - Wi-Fi, use
     - Brokered connection
     - The platform owns the radio; applications get sockets and an address.
   * - Wi-Fi credentials
     - Admin
     - Needs only ``APP_CAP_NET``. The credentials are device-wide and saved, so
       grant the network capability only to trusted applications.
   * - BLE scan
     - Open
     - One shared scan, results fanned out to every scanning application.
   * - BLE GATT client
     - Brokered connection, arbitrated pool
     - Per-application connections, capped by the controller's limit.
   * - Sockets
     - Brokered connection
     - One socket per application, isolated from the others.
   * - MQTT and friends
     - Brokered connection
     - The platform owns the client; arriving messages go to an event queue.

Sensors and analog
******************

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - Subsystem
     - Tier
     - Notes
   * - Sensor read
     - Open
     - By typed channel, shared and read-only.
   * - ADC read
     - Scoped
     - Per-application channel grant.
   * - ADC reference
     - Never exposed
     - Affects every reading on the device, so the platform keeps it.
   * - Battery status
     - Open
     - Read-only, with ``APP_CAP_POWER``.

Buses
*****

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - Subsystem
     - Tier
     - Notes
   * - I2C
     - Scoped
     - A granted address, never the raw bus.
   * - SPI
     - Scoped
     - The platform's SPI device, allowed per application. Chip selects are not
       scoped.
   * - UART
     - Capability only
     - The platform's secondary UART, gated by ``APP_CAP_UART`` with no
       per-application grant.
   * - CAN
     - Brokered connection
     - One bus and one receive queue shared by every application holding
       ``APP_CAP_CAN``, with no per-application identifier filter.
   * - GPIO
     - Scoped
     - An allowlist of lines, never every pin.

Storage
*******

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - Subsystem
     - Tier
     - Notes
   * - Filesystem
     - Namespaced-scoped
     - A per-application subtree, with traversal out of it refused.
   * - Key/value store
     - Namespaced-scoped
     - A per-application key space held in RAM and released when the
       application is reclaimed. It is not persisted.
   * - Flash
     - Namespaced-scoped
     - A granted window into a partition, never the whole device.
   * - Block device
     - Never exposed
     - Applications use the filesystem instead.

Time
****

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - Subsystem
     - Tier
     - Notes
   * - Read the time
     - Open
     - Shared and read-only.
   * - Set the hardware clock
     - Admin
     - Needs ``APP_CAP_RTC_SET``, a capability of its own.
   * - Set the platform wall clock
     - Admin
     - ``app_svc_rtc_set`` needs only ``APP_CAP_TIME``, and moves the clock every
       application reads.
   * - Software timers
     - Open
     - Expiry delivered to the application's queue.
   * - Hardware counter
     - Arbitrated pool
     - Scarce, so one granted instance with the limit enforced.
   * - PWM
     - Scoped
     - A granted channel from a fixed pool.
   * - System watchdog
     - Never exposed
     - The platform watches applications, not the reverse.

Platform foundation
*******************

These have no application surface. Reaching them would break isolation or the
device itself, so they are the platform's own responsibility:

* memory protection and memory domains
* clock control, pin control and cache
* DMA controllers, applications getting brokered transfers instead
* interrupt configuration and the scheduler
* the bootloader and updates of the base image
* power-management policy, applications having no power-management surface
