<!--
Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
SPDX-License-Identifier: Apache-2.0
-->

# Platform Capability Policy

This is the design map for a service-oriented application platform on Zephyr:
which subsystems are exposed to applications, how, and which are reserved for
the system. It is the curation layer - the platform is not "all of Zephyr
exposed", it is a deliberately classified subset. Every Zephyr subsystem sorts
into one of seven shapes; six of those shapes have a working reference
implementation in this sample series, so adding a new subsystem is
classification, not new mechanism.

## The seven shapes

| Shape | Meaning | Reference built |
|-------|---------|-----------------|
| **Open** | Read-only, shared, any app may call. | `sensor` catalog |
| **Scoped** | App granted one named instance via its manifest; broker enforces before any access. | scoped `i2c` |
| **Namespaced-scoped** | App gets a sandboxed subtree/keyspace it cannot escape. | `app_fs_sandbox` |
| **Brokered connection** | Stateful link the broker owns; events delivered to the app via its queue. | BLE, MQTT |
| **Arbitrated pool** | Scarce fixed pool (N instances); broker hands out, denies the N+1th. | BLE connections |
| **Admin** | System configuration; succeeds only for an app holding the admin capability. | Wi-Fi credentials |
| **Never exposed** | Platform foundation; no app surface at all. | (by omission) |

The capability tier an app sees collapses these to three: **open**, **scoped**
(covers scoped, namespaced-scoped, arbitrated-pool, brokered-connection - all
manifest-granted), and **admin**. "Never exposed" simply has no SDK symbol.

## The rule that drives classification

For each subsystem ask, in order:

1. **Does an app touching it endanger the system or other apps?** If yes and it
   cannot be safely scoped -> **Never exposed** (MPU, clocks, bootloader).
2. **Is it pure read-only shared state?** -> **Open** (sensor read, time read).
3. **Does it configure the whole device?** -> **Admin** (set time, set credentials).
4. **Is it a single resource an app should own a slice of?** -> **Scoped**
   (one i2c address, one UART, one counter instance).
5. **Is it storage?** -> **Namespaced-scoped** (own directory / keyspace).
6. **Is it a stateful link with async events?** -> **Brokered connection**.
7. **Is it a scarce hardware pool?** -> **Arbitrated pool**.

A single peripheral often splits by operation: reading is **Open**, configuring
is **Admin** (RTC, Wi-Fi, ADC reference).

## The policy table

### Connectivity / networking

| Subsystem | Tier | Exposure / notes |
|-----------|------|------------------|
| Wi-Fi (use) | Brokered connection | Supervisor owns the radio; apps get sockets/IP. |
| Wi-Fi credentials (set) | Admin | Persisted via `wifi_credentials`; `-EPERM` for normal apps. |
| BLE scan | Open (fan-out) | One shared scan, results to all scanning apps. |
| BLE GATT client | Brokered connection + Arbitrated pool | Per-app connections, `CONFIG_BT_MAX_CONN` cap. |
| BLE GATT server | Brokered | Apps register characteristics; broker muxes one table. |
| Sockets (TCP/UDP/TLS) | Brokered connection | Per-app socket, isolated by kobject permission. |
| MQTT / CoAP / WebSocket | Brokered connection | Broker owns client; incoming via app event queue. |
| LoRaWAN | Brokered connection + Admin | Session is brokered; keys/join config is admin. |
| 802.15.4 / Thread / Zigbee | Brokered + Admin | Same split: data brokered, network config admin. |

### Sensors / analog

| Subsystem | Tier | Exposure / notes |
|-----------|------|------------------|
| `sensor` (read) | Open | By typed channel; shared, read-only. |
| ADC (read) | Scoped | Per-app channel grant. |
| ADC reference / calibration | Admin | Affects all readings. |
| DAC | Scoped | Per-app channel grant. |
| Fuel gauge / battery | Open | Read-only status. |

### Buses (the scoped-bus pattern)

| Subsystem | Tier | Exposure / notes |
|-----------|------|------------------|
| I2C | Scoped | Manifest grants `(bus, address)`; never raw bus. |
| SPI | Scoped | Manifest grants `(bus, chip-select)`. |
| UART / serial | Scoped | Manifest grants a specific port. |
| CAN | Scoped | Manifest grants `(bus, message-ID filter)`. |
| 1-Wire / SMBus | Scoped | `(bus, device)` like I2C. |
| GPIO | Scoped | Manifest grants a pin allowlist; never all pins. |

### Storage (the namespaced-scoped pattern)

| Subsystem | Tier | Exposure / notes |
|-----------|------|------------------|
| Filesystem / SD card | Namespaced-scoped | Each app gets `/apps/<id>/...`; broker confines to the subtree, blocks path traversal. |
| NVS / settings | Namespaced-scoped | Per-app key prefix; an app cannot read another app's keys or system settings. |
| Flash (raw) | Never exposed (system) / Namespaced | Raw flash never; a flash-backed app partition only. |
| Disk access (block) | Never exposed | Apps use the filesystem, not the block device. |

### Time / timers / scheduling

| Subsystem | Tier | Exposure / notes |
|-----------|------|------------------|
| RTC (read time) | Open | `app_time_now()`; shared, read-only. |
| RTC (set time) | Admin | Affects the whole device. |
| Software timers (`k_timer`) | Open (brokered event) | Unlimited; expiry delivered to the app's event queue. |
| Hardware counter | Arbitrated pool | Scarce; manifest grants one instance, N-max enforced. |
| PWM | Scoped / Arbitrated pool | Per-app channel grant from a fixed pool. |
| Watchdog (system) | Never exposed | The supervisor watches apps, not the reverse. |

### Human interface

| Subsystem | Tier | Exposure / notes |
|-----------|------|------------------|
| Input (buttons/touch) | Brokered | Routed to the foreground app only. |
| Display / LVGL | Brokered (compositor) | Broker owns the screen; each app owns a screen object it draws into; one foreground at a time. |
| LED | Scoped / Open | Per-app LED grant. |
| Audio (I2S/codec) | Scoped + Arbitrated | One stream owner at a time; brokered mixing if shared. |

### Acceleration / misc

| Subsystem | Tier | Exposure / notes |
|-----------|------|------------------|
| Crypto (PSA) | Open | Stateless ops; per-app key handles for keyed ops. |
| RNG | Open | Read-only entropy. |
| USB device stack | Never exposed (system) | Device-role config is platform-level. |
| USB host (per device) | Scoped | A granted attached device, like a bus device. |

### Platform foundation - never exposed

These have **no application surface**. Touching them is, by definition, the
platform's job; an app reaching them would break isolation or the device.

- Memory protection (MPU / PMP / MMU), memory domains
- Clock control, PLLs, `pinctrl`, `regi2c`, cache
- DMA controllers (apps get brokered transfers, never the controller)
- Interrupt configuration, the scheduler internals
- Bootloader / MCUboot, firmware update of the base image
- Power-management policy (apps express hints; the system decides)

## What "addressing it all" means

The work is not hundreds of driver wrappers. It is:

1. **One wrapper per subsystem class** (~20), each riding Zephyr's generic API
   so it covers every chip of that class for free.
2. **This policy table** - the tier decision per class. The "never exposed" rows
   are zero work (omission). The admin and open rows are thin. The scoped and
   namespaced rows carry the manifest-enforcement logic.
3. **Manifest-derived scoping from devicetree** - the base firmware's DT already
   describes what hardware exists; an app's manifest claims a subset, validated
   against DT, so per-app grants are partly generated, not hand-written.

The platform's *mechanisms* are complete - all seven shapes are proven on
hardware. Extending to a new subsystem is choosing its row above, then writing
one wrapper in the already-proven shape.
