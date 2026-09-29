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
identity only (name, version, module hash and ABI), not its capabilities.

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
