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
     - Read-only, shared; any application may call.
     - sensor catalog
   * - Scoped
     - One named instance granted through the manifest, enforced before access.
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
     - Device-wide configuration, for an application holding the admin
       capability.
     - Wi-Fi credentials
   * - Never exposed
     - Platform foundation, with no application surface at all.
     - by omission

What an application sees collapses these to three tiers: open, scoped (covering
scoped, namespaced-scoped, arbitrated pool and brokered connection, all granted
through the manifest) and admin. A never-exposed subsystem simply has no
service symbol.

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
is admin. The real-time clock, Wi-Fi and the ADC reference all divide that way.

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
     - Refused with ``-EPERM`` for an ordinary application.
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
     - Admin
     - Affects every reading on the device.
   * - Battery status
     - Open
     - Read-only.

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
     - A granted bus and chip select.
   * - UART
     - Scoped
     - A granted port.
   * - CAN
     - Scoped
     - A granted bus and identifier filter.
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
   * - Settings
     - Namespaced-scoped
     - A per-application key prefix.
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
   * - Set the time
     - Admin
     - Affects the whole device.
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
* power-management policy, to which applications contribute only hints
