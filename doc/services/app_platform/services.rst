.. _app_platform_services_catalog:

Service Catalog
###############

Applications reach the system only through the platform services. This page
lists every service, the capability that gates it, the per-application grant
that scopes it, the Kconfig option that builds it, the calls an application
makes and a sample that shows it, and then describes how each one works. The
classification behind each service is described in :ref:`app_platform_policy`.

How a service call works
************************

Every service follows the same steps:

#. **Capability.** The service checks the capability mask of the calling
   application, taken from the calling thread, and returns ``-EPERM`` when the
   bit is missing. A thread that is not part of an application holds no
   capability.
#. **Arguments.** Indexes, lengths and flags are checked next, with
   ``-EINVAL`` for a value out of range. With user mode, every buffer an
   application passes is checked against its memory domain, so it can hand a
   service only memory it can reach itself.
#. **Grant.** A scoped service then checks that the line, channel, address,
   window or instance was granted to the calling application, and returns
   ``-EACCES`` when it was not. Grants are made by the firmware with the
   ``app_platform_grant_*`` functions, usually in the configure hook, see
   :ref:`app_platform`.
#. **Device.** A service whose device is missing or not ready returns
   ``-ENODEV``.
#. **Work.** The service does the work under its own lock. A lock that cannot
   be taken within two seconds returns ``-EBUSY``, so a wedged resource never
   parks the caller.

State a service keeps for an application, such as a queue, a socket or a
timer, is indexed by the application's slot. When the application stops, is
removed or is reclaimed after a fault or a hang, the reaper runs the reclaim
hook of every service and clears the application's grants. Each service then
releases what the application held and drives the outputs it left active back
to idle. Grants are dropped on every stop, so the firmware sets them again
before each start, in a configure hook.

Services that receive data in the background announce it through the
application's event queue, see `Events`_.

Catalog
*******

The capability names drop the ``APP_CAP_`` prefix and the Kconfig options the
``CONFIG_APP_PLATFORM_`` prefix. Every service option depends on
:kconfig:option:`CONFIG_APP_PLATFORM_SERVICES`.

.. list-table::
   :header-rows: 1
   :widths: 14 14 18 14 22 18

   * - Service
     - Capability
     - Grant
     - Kconfig
     - Calls
     - Sample
   * - Log, heartbeat
     - ``LOG``; none for the heartbeat
     - -
     - ``SERVICES``
     - ``app_svc_log``, ``app_svc_heartbeat``, ``app_svc_stop_requested``
     - :zephyr_file:`samples/subsys/app_liveness`
   * - Hash, random
     - ``CRYPTO``, ``RANDOM``
     - -
     - ``SERVICES``
     - ``app_svc_sha256``, ``app_svc_random``
     - :zephyr:code-sample:`app-probe`
   * - Time, timers, timer slots
     - ``TIME``
     - -
     - ``SERVICES``
     - ``app_svc_uptime_ms``, ``app_svc_cycles``, ``app_svc_timer_*``,
       ``app_svc_rtc_now``, ``app_svc_rtc_set``, ``app_svc_device_id``,
       ``app_svc_reset_cause``
     - :zephyr:code-sample:`app-timer-slots`
   * - Key/value store
     - ``KV``
     - -
     - ``SERVICES``
     - ``app_svc_kv_*``
     - :zephyr:code-sample:`app-kv-store`
   * - Flash window
     - ``FLASH``
     - ``app_platform_grant_flash``
     - ``SERVICES``
     - ``app_svc_flash_*``
     - :zephyr:code-sample:`app-probe`
   * - IPC and topics
     - ``IPC``
     - -
     - ``SERVICES``
     - ``app_svc_ipc_*``, ``app_svc_topic_*``
     - :zephyr_file:`samples/subsys/app_trio`
   * - Inspect, control
     - ``INSPECT``, ``CONTROL``
     - -
     - ``SERVICES``
     - ``app_svc_app_count``, ``app_svc_app_info``, ``app_svc_app_stop``,
       ``app_svc_app_start``, ``app_svc_app_restart``
     - :zephyr_file:`samples/subsys/app_inspect`
   * - Threads, semaphores
     - ``THREAD``
     - -
     - ``APP_THREADS``, ``APP_SEMS``
     - ``app_svc_thread_*``, ``app_svc_sem_*``
     - :zephyr:code-sample:`app-workers`
   * - Heap
     - none
     - -
     - ``APP_HEAP_POOL_SIZE``
     - ``app_malloc`` and friends, ``app_svc_heap_info``
     - :zephyr:code-sample:`app-heap`
   * - Events
     - ``INPUT``
     - -
     - ``EVENTS``
     - ``app_svc_event_wait``, ``app_svc_event_subscribe``
     - :zephyr_file:`samples/subsys/app_dispatch`
   * - GPIO
     - ``GPIO``
     - ``app_platform_grant_gpio``
     - ``GPIO``
     - ``app_svc_gpio_*``
     - :zephyr_file:`samples/subsys/app_dispatch`
   * - LEDs
     - ``LED``
     - ``app_platform_grant_led``
     - ``IO``
     - ``app_svc_led_*``
     - :zephyr:code-sample:`app-io`
   * - Buttons
     - ``INPUT``
     - -
     - ``IO``, ``INPUT_KEYS``
     - ``app_svc_button_wait``
     - :zephyr:code-sample:`app-io`
   * - Checkpoint
     - none
     - -
     - ``IO``
     - ``app_svc_checkpoint_set``, ``app_svc_checkpoint_get``
     - :zephyr_file:`samples/subsys/app_recovery`
   * - PWM
     - ``PWM``
     - ``app_platform_grant_pwm``
     - ``PERIPH``
     - ``app_svc_pwm_*``
     - :zephyr:code-sample:`app-probe`
   * - ADC
     - ``ADC``
     - ``app_platform_grant_adc``
     - ``PERIPH``
     - ``app_svc_adc_*``
     - :zephyr:code-sample:`app-probe`
   * - DAC
     - ``DAC``
     - ``app_platform_grant_dac``
     - ``DAC``
     - ``app_svc_dac_*``
     - :zephyr_file:`tests/subsys/app_platform_devices`
   * - EEPROM
     - ``EEPROM``
     - ``app_platform_grant_eeprom``
     - ``EEPROM``
     - ``app_svc_eeprom_*``
     - :zephyr_file:`tests/subsys/app_platform_devices`
   * - Sensors
     - ``SENSOR``
     - -
     - ``SENSOR``
     - ``app_svc_sensor_*``, ``app_svc_die_temp_milli``
     - :zephyr:code-sample:`app-probe`
   * - Hardware counter
     - ``COUNTER_DEV``
     - ``app_platform_grant_counter``
     - ``COUNTER``
     - ``app_svc_counter_*``
     - :zephyr_file:`tests/subsys/app_platform`
   * - Pulse counter, motor PWM
     - ``COUNTER``, ``MOTOR``
     - -
     - ``MOTION``
     - ``app_svc_pulse_emit``, ``app_svc_pulse_count``, ``app_svc_motor_set``
     - :zephyr_file:`samples/subsys/app_matrix`
   * - Real-time clock
     - ``RTC``, ``RTC_SET``
     - -
     - ``RTC``
     - ``app_svc_rtcdev_get``, ``app_svc_rtcdev_set``
     - :zephyr_file:`tests/subsys/app_platform_devices`
   * - 1-Wire
     - ``W1``
     - -
     - ``W1``
     - ``app_svc_w1_*``
     - -
   * - Power
     - ``POWER``
     - -
     - ``POWER``
     - ``app_svc_charger_online``, ``app_svc_battery_percent``
     - -
   * - Regulator
     - ``REGULATOR``
     - ``app_platform_grant_regulator``
     - ``REGULATOR``
     - ``app_svc_regulator_*``
     - :zephyr_file:`tests/subsys/app_platform_devices`
   * - Stepper
     - ``STEPPER``
     - ``app_platform_grant_stepper``
     - ``STEPPER``
     - ``app_svc_stepper_*``
     - :zephyr_file:`tests/subsys/app_platform_devices`
   * - RGB LEDs
     - ``RGB``
     - ``app_platform_grant_rgb``
     - ``RGB``
     - ``app_svc_rgb_*``
     - :zephyr:code-sample:`app-rgb`
   * - Display
     - ``DISPLAY``
     - ``app_platform_grant_display``
     - ``DISPLAY``
     - ``app_svc_display_*``
     - :zephyr:code-sample:`app-display`
   * - I2C
     - ``I2C``
     - ``app_platform_grant_i2c``
     - ``BUS``
     - ``app_svc_i2c_*``
     - :zephyr:code-sample:`app-probe`
   * - SPI
     - ``SPI``
     - ``app_platform_grant_spi``, ``app_platform_grant_spi_cs``
     - ``BUS``
     - ``app_svc_spi_*``
     - :zephyr:code-sample:`app-probe`
   * - Secondary UART
     - ``UART``
     - -
     - ``BUS``
     - ``app_svc_uart_*``
     - :zephyr:code-sample:`app-uart-line`
   * - Ring
     - ``UART``
     - -
     - ``RING``
     - ``app_svc_ring_*``
     - :zephyr:code-sample:`app-ring`
   * - CAN
     - ``CAN``
     - -
     - ``CAN``
     - ``app_svc_can_*``
     - :zephyr:code-sample:`app-can`
   * - 802.15.4 radio
     - ``RADIO``
     - -
     - ``RADIO``
     - ``app_svc_radio_send``, ``app_svc_radio_recv``
     - :zephyr:code-sample:`app-probe`
   * - DMA copy
     - ``DMA``
     - -
     - ``DMA``
     - ``app_svc_dma_copy``
     - :zephyr:code-sample:`app-probe`
   * - External RAM
     - ``PSRAM``
     - -
     - ``PSRAM``
     - ``app_svc_psram_map``, ``app_svc_psram_unmap``
     - :zephyr_file:`samples/subsys/psram_two_apps`
   * - Files
     - ``FS``
     - -
     - ``FS``
     - ``app_svc_file_*``
     - :zephyr_file:`tests/subsys/app_platform`
   * - Wi-Fi link, MQTT
     - ``NET``
     - -
     - ``NET``
     - ``app_svc_net_ready``, ``app_svc_wifi_*``, ``app_svc_mqtt_*``
     - :zephyr_file:`samples/subsys/app_trio`
   * - UDP and TCP sockets
     - ``SOCKET``
     - -
     - ``NET``
     - ``app_svc_resolve``, ``app_svc_udp_*``, ``app_svc_tcp_*``
     - :zephyr_file:`samples/subsys/app_wifi_scenarios`
   * - BLE scan, heart rate
     - ``BLE``
     - -
     - ``BLE``
     - ``app_svc_ble_scan``, ``app_svc_ble_hr_read``
     - :zephyr:code-sample:`app-probe`
   * - BLE GATT client
     - ``BLE``
     - ``app_platform_grant_ble_gatt``
     - ``BLE_GATT``
     - ``app_svc_ble_gatt_read``, ``app_svc_ble_gatt_write``
     - :zephyr:code-sample:`app-ble-gatt`
   * - BLE GATT server
     - ``BLE``
     - ``app_platform_grant_ble_gatt``
     - ``BLE_SERVE``
     - ``app_svc_ble_serve_*``, ``app_svc_ble_notify``,
       ``app_svc_ble_is_connected``, ``app_svc_ble_advertise``
     - :zephyr:code-sample:`app-ble-peripheral`
   * - BLE application fetch
     - ``BLE``
     - -
     - ``BLE_OTA``
     - ``app_svc_ble_ota_fetch``
     - :zephyr_file:`samples/subsys/app_ble_ota`

The registry, the loader and the ``app`` shell are firmware interfaces, not
application services; they are described at the end of this page.

Core services
*************

Log and liveness
================

:c:func:`app_svc_log` writes a line tagged with the application's slot to the
``app_log`` log module. It needs ``APP_CAP_LOG``; a call without it is dropped.
:c:func:`app_platform_log_enable` and the ``app log`` shell command mute and
unmute every application's output at once, for example while an operator types
on the console. :c:func:`app_svc_heartbeat` needs no capability and feeds the
watchdog channel of a monitored application, see
:ref:`app_platform_liveness`. :c:func:`app_svc_stop_requested` returns 1 once
the firmware asked the application to exit with
:c:func:`app_platform_request_stop`, so it can return from its entry point
cleanly.

Hash and random
===============

:c:func:`app_svc_sha256` hashes up to 256 bytes with PSA crypto and needs
``APP_CAP_CRYPTO``. :c:func:`app_svc_random` fills up to 256 bytes from the
cryptographically secure generator when the system has one, and needs
``APP_CAP_RANDOM``. Both return ``-ENOTSUP`` when the firmware has no provider.

Time and timers
===============

``APP_CAP_TIME`` gates the time services:

* :c:func:`app_svc_uptime_ms` and :c:func:`app_svc_cycles` read the uptime and
  the cycle counter.
* :c:func:`app_svc_timer_start` and :c:func:`app_svc_timer_wait` give each
  application one one-shot timer.
* The timer slots give each application four independent timers.
  :c:func:`app_svc_timer_create` takes a slot, :c:func:`app_svc_timer_arm`
  starts it one-shot or periodic (0 ms stops it, and arming a running slot
  restarts it), :c:func:`app_svc_timer_slot_wait` waits for it, and
  :c:func:`app_svc_timer_stop`, :c:func:`app_svc_timer_elapsed` and
  :c:func:`app_svc_timer_free` complete the set. Expiries that are not taken
  coalesce into one, and each expiry posts ``APP_EVENT_TIMER`` with the slot
  number to a subscribed application. The slots of an application are stopped
  and freed when it is reclaimed.
* :c:func:`app_svc_rtc_now` and :c:func:`app_svc_rtc_set` read and set the
  platform wall clock, a seed plus the uptime held in RAM and shared by every
  application. The hardware clock is a separate service, see
  `Real-time clock`_.
* :c:func:`app_svc_device_id` and :c:func:`app_svc_reset_cause` read the
  hardware information driver.

Key/value store
===============

``APP_CAP_KV`` gives an application a key space of ``uint32_t`` values in RAM:
:c:func:`app_svc_kv_set`, :c:func:`app_svc_kv_get` and
:c:func:`app_svc_kv_delete`. The entries come from one pool of sixteen shared by
all applications, keyed by owner, so two applications can use the same key name
without seeing each other's value. A full pool returns ``-ENOSPC``. The store is
not persisted, and an application's keys are deleted when it stops or is
reclaimed.

Flash window
============

``APP_CAP_FLASH`` gives an application raw access to a window of the
``storage_partition`` that the firmware grants with
:c:func:`app_platform_grant_flash`, a base and a length, 0 to revoke.
:c:func:`app_svc_flash_read`, :c:func:`app_svc_flash_write` and
:c:func:`app_svc_flash_erase` take offsets relative to the window and move up to
256 bytes per call. An access outside the window, or without one, returns
``-EACCES``. The window is dropped when the application stops.

IPC and topics
==============

``APP_CAP_IPC`` gives each application a mailbox of four messages of up to 64
bytes. :c:func:`app_svc_ipc_send` addresses a slot,
:c:func:`app_svc_ipc_register` names the caller and
:c:func:`app_svc_ipc_send_to` addresses an application by name.
:c:func:`app_svc_ipc_recv` reads the caller's own mailbox. A full mailbox
returns ``-EAGAIN``. With :c:func:`app_svc_topic_subscribe` an application
follows one topic, and :c:func:`app_svc_topic_publish` copies a message into
the mailbox of every subscriber, skipping full ones, and returns how many
received it. Every delivery posts ``APP_EVENT_IPC`` to a subscribed
application. The mailbox, the name and the topic are cleared when the
application stops.

Inspect and control
===================

``APP_CAP_INSPECT`` lets an application observe the others:
:c:func:`app_svc_app_count` returns the number of slots and
:c:func:`app_svc_app_info` reports the name, state, stack use and CPU cycles of
one, as :c:func:`app_platform_status` does for the firmware.
``APP_CAP_CONTROL`` lets it stop, start and restart other applications with
:c:func:`app_svc_app_stop`, :c:func:`app_svc_app_start` and
:c:func:`app_svc_app_restart`, but never itself (``-EINVAL``). An application
the firmware launched stopped and has not started yet is held, and starting it
returns ``-EAGAIN``. These capabilities suit a launcher or a monitor
application; see :zephyr_file:`samples/subsys/app_top`.

Threads and semaphores
======================

``APP_CAP_THREAD`` gates :c:func:`app_svc_thread_spawn`,
:c:func:`app_svc_thread_join` and the semaphores. Workers run in user mode in
the application's domain, at its priority plus an offset, and are reserved per
slot by :kconfig:option:`CONFIG_APP_PLATFORM_APP_THREADS`; a spawn with no
worker free returns ``-EAGAIN``. Semaphores are handles into the caller's own
table of :kconfig:option:`CONFIG_APP_PLATFORM_APP_SEMS` entries, so one
application can never name another's. See :ref:`app_platform` for the thread
model.

Heap
====

The heap needs no capability: an application declares it, in its signed
manifest or in :c:member:`app_def.heap_size`, and the allocator runs in the
application's own thread. :c:func:`app_svc_heap_info` returns the region. See
:ref:`app_platform` for the pool and the mapping.

Events
======

``APP_CAP_INPUT`` gives an application an event queue of
:kconfig:option:`CONFIG_APP_PLATFORM_EVENT_QUEUE_LEN` entries, enabled with
:kconfig:option:`CONFIG_APP_PLATFORM_EVENTS`. :c:func:`app_svc_event_wait`
takes the next :c:struct:`app_event`, which carries its kind, a line, a value,
a code and a sequence number. Events are posted without blocking, also from
interrupts; when the queue is full the oldest event is dropped and the sequence
number skips to show it.

Two kinds of events reach the queue:

* Input events are always delivered: ``APP_EVENT_BUTTON``, ``APP_EVENT_GPIO``
  for a watched line, ``APP_EVENT_GATT_WRITE``, ``APP_EVENT_BLE_CONNECTED``,
  ``APP_EVENT_BLE_DISCONNECTED`` and ``APP_EVENT_STOP``.
* Notifications are delivered only for the kinds the application subscribed to
  with :c:func:`app_svc_event_subscribe`, a mask of ``APP_EVENT_BIT()`` values:
  ``APP_EVENT_TIMER``, ``APP_EVENT_IPC``, ``APP_EVENT_CAN``,
  ``APP_EVENT_UART_RX``, ``APP_EVENT_NET`` (line 0 for MQTT, line 1 for the
  socket), ``APP_EVENT_COUNTER`` and ``APP_EVENT_RADIO``. A notification says
  data is waiting and is not queued again while one of the same kind and line
  is pending; the application drains the source with a zero timeout until it
  returns ``-EAGAIN``.

:zephyr_file:`include/zephyr/app_platform/app_dispatch.h` wraps the queue in a
callback loop: :c:func:`app_dispatch_on` registers a handler per event kind,
:c:func:`app_dispatch_run` waits and calls the handlers on the application's own
thread, and :c:func:`app_dispatch_stop` ends the loop. It works in built-in
applications and in modules. The queue and the subscription are cleared when
the application stops.

Input and output
****************

GPIO
====

The lines are the ``gpios`` of the ``zephyr,app-platform`` devicetree node, and
:c:func:`app_platform_grant_gpio` grants a mask of line indexes.
:c:func:`app_svc_gpio_count` and :c:func:`app_svc_gpio_find`, which looks up a
name from ``line-names``, need only ``APP_CAP_GPIO``. :c:func:`app_svc_gpio_set`,
:c:func:`app_svc_gpio_get` and :c:func:`app_svc_gpio_configure` act on one
granted line, and :c:func:`app_svc_gpio_set_mask` and
:c:func:`app_svc_gpio_get_mask` on several, each line checked before any
changes. :c:func:`app_svc_gpio_configure` sets the direction, the pulls and
open drain.

:c:func:`app_svc_gpio_watch` and :c:func:`app_svc_gpio_watch_edge` turn a
granted line into an input with an edge interrupt and post ``APP_EVENT_GPIO``
with the line and its level to every watching application whose edge filter
matches, with a debounce window per application and line. A line can be
granted to several applications; the platform records which one last drove it
active. On reclaim the application's watches are removed, lines it configured
return to output-inactive unless another application watches them, and lines
it last drove active are set to 0.

LEDs, buttons and the checkpoint
================================

The LEDs are the ``led0`` to ``led2`` aliases, named by their ``label``.
:c:func:`app_platform_grant_led` grants a mask of LED indexes and
:c:func:`app_svc_led_set` drives one with ``APP_CAP_LED``; reclaim turns off the
LEDs the application last turned on.

:c:func:`app_svc_button_wait` needs ``APP_CAP_INPUT`` and waits for a press.
With :kconfig:option:`CONFIG_APP_PLATFORM_INPUT_KEYS` the presses come from the
input subsystem and carry the key code; otherwise they come from the ``sw0``
alias. Every press is also broadcast as ``APP_EVENT_BUTTON``.

:c:func:`app_svc_checkpoint_set` and :c:func:`app_svc_checkpoint_get` keep one
``uint32_t`` per application name outside every memory domain, with no
capability. The value survives a fault and a relaunch, so a resilient
application can resume where it stopped; see
:zephyr_file:`samples/subsys/app_recovery`.

PWM and ADC
===========

The PWM channels are the ``app-pwm0`` to ``app-pwm7`` aliases. With
``APP_CAP_PWM`` and a channel granted by :c:func:`app_platform_grant_pwm`,
:c:func:`app_svc_pwm_set` sets a duty cycle in per mille of the devicetree
period and :c:func:`app_svc_pwm_set_ns` sets an explicit period and pulse.
Reclaim sets the channels the application last drove to zero duty.

The ADC channels are the ``io-channels`` of the ``/zephyr,user`` node, set up on
first use. With ``APP_CAP_ADC`` and a channel granted by
:c:func:`app_platform_grant_adc`, :c:func:`app_svc_adc_read` returns the raw
sample and :c:func:`app_svc_adc_read_mv` the value in millivolts. The reference
is never exposed.

DAC
===

The DACs are the ``dacs`` of the ``zephyr,app-platform`` node, channel 0 of
each. :c:func:`app_platform_grant_dac` grants a mask of indexes, and
:c:func:`app_svc_dac_write` writes a value of
:kconfig:option:`CONFIG_APP_PLATFORM_DAC_RESOLUTION` bits with
``APP_CAP_DAC``. Reclaim writes 0 to the channels the application last set.

EEPROM
======

The EEPROM is the ``zephyr,eeprom`` chosen device.
:c:func:`app_platform_grant_eeprom` grants a byte window, and
:c:func:`app_svc_eeprom_size`, :c:func:`app_svc_eeprom_read` and
:c:func:`app_svc_eeprom_write` take offsets relative to it, up to
:kconfig:option:`CONFIG_APP_PLATFORM_EEPROM_XFER_MAX` bytes per call. The
firmware assigns disjoint windows to keep applications apart. The stored bytes
stay when the application is reclaimed.

Sensors
=======

The sensor catalog is a fixed list of 35 kinds, such as ``temperature``,
``humidity``, ``accel-x`` or ``batt-voltage``, the same on every board.
:c:func:`app_svc_sensor_count` and :c:func:`app_svc_sensor_info` describe the
kinds and whether the board backs them. The devices are the ``sensors`` of the
``zephyr,app-platform`` node: on first use each is fetched once, and the first
that reports a kind's channel backs that kind; the ``die-temp0`` alias backs
``temperature`` when no listed device does. :c:func:`app_svc_sensor_read`
returns one reading in milli-units and :c:func:`app_svc_sensor_read_n` up to
:kconfig:option:`CONFIG_APP_PLATFORM_SENSOR_READ_N_MAX` readings under one hold
of the device. Reading is open and shared: ``APP_CAP_SENSOR`` is the only
check. :c:func:`app_svc_die_temp_milli` reads the die temperature with the same
capability.

Hardware counter
================

The counters are the ``counters`` of the ``zephyr,app-platform`` node, granted
by :c:func:`app_platform_grant_counter` and used with ``APP_CAP_COUNTER_DEV``:
:c:func:`app_svc_counter_start`, :c:func:`app_svc_counter_stop`,
:c:func:`app_svc_counter_read` and :c:func:`app_svc_counter_frequency`.
:c:func:`app_svc_counter_alarm` sets an alarm a number of ticks ahead, which
posts ``APP_EVENT_COUNTER`` with the counter index to a subscribed application.
An alarm channel belongs to one application at a time, and a new alarm replaces
the caller's pending one. Reclaim cancels the application's alarms and stops
the counters it last started.

Pulse counter and motor PWM
===========================

:kconfig:option:`CONFIG_APP_PLATFORM_MOTION` exposes a pulse counter, the
``pcnt`` node read as a rotation sensor, with ``APP_CAP_COUNTER``:
:c:func:`app_svc_pulse_count` reads it and :c:func:`app_svc_pulse_emit` drives a
burst of pulses on the ``pcnt-stim`` PWM wired to its input. With
``APP_CAP_MOTOR``, :c:func:`app_svc_motor_set` sets the duty of one of the two
channels of the ``mcpwm0`` motor-control PWM. Reclaim sets the motor channels
the application last drove to zero duty.

Real-time clock
===============

The hardware clock is the ``zephyr,rtc`` chosen device. Reading it with
:c:func:`app_svc_rtcdev_get` needs ``APP_CAP_RTC``, and setting it with
:c:func:`app_svc_rtcdev_set` needs ``APP_CAP_RTC_SET``, a capability of its own
because the change is device-wide. The time set stays when the application is
reclaimed.

1-Wire and power
================

:c:func:`app_svc_w1_reset`, :c:func:`app_svc_w1_read_byte` and
:c:func:`app_svc_w1_write_byte` reach the ``zephyr,w1`` chosen bus with
``APP_CAP_W1``. With ``APP_CAP_POWER``, :c:func:`app_svc_charger_online` and
:c:func:`app_svc_battery_percent` read the ``zephyr,charger`` and
``zephyr,fuel-gauge`` chosen devices; the service is read-only.

Regulators
==========

The regulators are the ``regulators`` of the ``zephyr,app-platform`` node,
granted by :c:func:`app_platform_grant_regulator`.
:c:func:`app_svc_regulator_enable` enables or disables one with
``APP_CAP_REGULATOR``. Enables are counted per application, so an application
can release only an enable it holds (``-EALREADY`` otherwise), and an output
stays on while any application holds it. Reclaim releases every enable the
application holds.

Steppers
========

The stepper controllers are the ``steppers`` of the ``zephyr,app-platform``
node, granted by :c:func:`app_platform_grant_stepper`. With
``APP_CAP_STEPPER``, :c:func:`app_svc_stepper_move_by` starts a relative move,
:c:func:`app_svc_stepper_stop` stops it and :c:func:`app_svc_stepper_position`
reads the position, which the application polls to follow a move. Reclaim stops
the steppers on which the application issued the last move.

Addressable RGB LEDs
====================

The strip is the ``led-strip`` alias, up to 32 pixels.
:c:func:`app_platform_grant_rgb` grants a mask of pixels, and
:c:func:`app_svc_rgb_set` sets one with ``APP_CAP_RGB``. The platform keeps the
colors of the whole strip and refreshes it on every change, so each
application changes only its own pixels while several share one strip. Reclaim
blacks out the pixels the application last lit.

Display
=======

The panel is the ``zephyr,display`` chosen device.
:c:func:`app_platform_grant_display` grants a viewport, a rectangle of the
panel, and the application draws inside it with ``APP_CAP_DISPLAY`` in
viewport coordinates: :c:func:`app_svc_display_info` reports its size and the
pixel format, :c:func:`app_svc_display_write` copies pixels,
:c:func:`app_svc_display_fill` fills a rectangle with one color and
:c:func:`app_svc_display_vsync` waits for the panel's vertical sync. Pixels are
staged through a per-application buffer of
:kconfig:option:`CONFIG_APP_PLATFORM_DISPLAY_XFER_MAX` bytes and written under
the display lock, which firmware code that draws on the same panel takes with
:c:func:`app_platform_display_lock`. A rectangle outside the viewport returns
``-ERANGE``. Reclaim fills the viewport of an application that drew with
:kconfig:option:`CONFIG_APP_PLATFORM_DISPLAY_CLEAR_COLOR`.

Buses
*****

I2C
===

The bus is the ``app-i2c`` alias. :c:func:`app_platform_grant_i2c` grants a
7-bit address, up to :kconfig:option:`CONFIG_APP_PLATFORM_I2C_ADDRS` per
application, and an address belongs to one application at a time, so a second
grant of it returns ``-EBUSY``. With ``APP_CAP_I2C``,
:c:func:`app_svc_i2c_read`, :c:func:`app_svc_i2c_write`,
:c:func:`app_svc_i2c_write_read` and :c:func:`app_svc_i2c_transfer`, up to eight
messages sliced from one buffer, reach only granted addresses, and never the raw
bus.

Each application has its own bus speed. :c:func:`app_svc_i2c_configure` stores
the speed for the calling application only, and the service programs the
controller before a transfer whose speed differs from the last one, under the
bus lock, so one application's speed never applies to another's transfer. Failed
transfers in a row are counted per application; at
:kconfig:option:`CONFIG_APP_PLATFORM_I2C_ERROR_LIMIT` the application loses all
of its addresses and the bus is recovered. On stop the addresses, the error
count and the speed are cleared.

SPI
===

The controller is the ``app-spi`` alias. :c:func:`app_platform_grant_spi` allows
an application to use the controller, and :c:func:`app_platform_grant_spi_cs`
grants a chip select, an index into the ``cs-gpios`` of the controller or its
native chip select, to one application at a time. Each application has its own
settings, 1 MHz, mode 0 and 8-bit words until it calls
:c:func:`app_svc_spi_configure` with a frequency, the clock polarity and phase,
the bit order, the word size and one of its chip selects. They apply to its own
:c:func:`app_svc_spi_xfer` and :c:func:`app_svc_spi_transceive` calls only. On
stop the grants are dropped and the settings return to the defaults.

Secondary UART
==============

The port is the ``app-uart`` chosen device, shared by every application holding
``APP_CAP_UART``. :c:func:`app_svc_uart_write` and :c:func:`app_svc_uart_read`
move bytes; with interrupt-driven UART support reception fills one receive
buffer of :kconfig:option:`CONFIG_APP_PLATFORM_UART_RX_BUF` bytes and posts
``APP_EVENT_UART_RX`` to subscribed applications. :c:func:`app_svc_uart_errors`
returns and clears the port's error bits. The first application to change the
line settings with :c:func:`app_svc_uart_configure` or
:c:func:`app_svc_uart_set_flow` owns them until it calls
:c:func:`app_svc_uart_close` or is reclaimed, when the boot settings are
restored; others get ``-EBUSY`` meanwhile.

Ring
====

:kconfig:option:`CONFIG_APP_PLATFORM_RING` gives an application holding
``APP_CAP_UART`` a byte ring on the secondary UART that it fills and drains
without a call per byte. :c:func:`app_svc_ring_bind` allocates a buffer of a
power of two bytes, up to :kconfig:option:`CONFIG_APP_PLATFORM_RING_MAX_BYTES`,
and maps it into the application's domain; it starts with a header holding the
head and tail indexes. :c:func:`app_svc_ring_flush` sends the bytes between
them and :c:func:`app_svc_ring_fill` receives into the free space, each in one
call. The indexes are checked on every call, and a ring with indexes out of
range returns ``-EBADMSG``. :c:func:`app_svc_ring_unbind`, a stop or a reclaim
unmaps and frees the buffer.

CAN
===

The controller is the ``zephyr,canbus`` chosen device, started on first use,
in loopback with :kconfig:option:`CONFIG_APP_PLATFORM_CAN_LOOPBACK`. The bus is
shared by every application holding ``APP_CAP_CAN``. Each application adds up
to :kconfig:option:`CONFIG_APP_PLATFORM_CAN_FILTERS` identifier filters with
:c:func:`app_svc_can_filter_add` and receives the matching frames, or every
frame without a filter, into its own queue of
:kconfig:option:`CONFIG_APP_PLATFORM_CAN_RX_DEPTH` frames, read with
:c:func:`app_svc_can_recv` or :c:func:`app_svc_can_recv_frame`. A frame that
finds the queue full is dropped and counted. :c:func:`app_svc_can_send` and
:c:func:`app_svc_can_send_frame` send classic CAN frames, and
:c:func:`app_svc_can_status` reports the bus state, the error counters, the
last transmit error and the drops. Each queued frame posts ``APP_EVENT_CAN``
to a subscribed application. On stop the filters and the queue are cleared.

802.15.4 radio
==============

The platform owns one UDP socket over IPv6 on the 802.15.4 interface, bound to
port 4242 and sending to the peer in
:kconfig:option:`CONFIG_APP_PLATFORM_RADIO_PEER`. Platform threads do all
socket work. :c:func:`app_svc_radio_send` queues a datagram of up to 64 bytes
for transmission, and returns ``-EAGAIN`` when the transmit queue is full.
Each application that has used the radio has its own receive queue of four
datagrams: a received datagram is copied into the queue of every radio
application, and one that finds a queue full is dropped for that application
only. :c:func:`app_svc_radio_recv` reads the caller's queue, and each queued
datagram posts ``APP_EVENT_RADIO`` to a subscribed application. On stop the
application's queue is purged.

DMA copy
========

:c:func:`app_svc_dma_copy` copies up to 256 bytes with a memory-to-memory
transfer on the ``dma`` controller, with ``APP_CAP_DMA``. The platform copies
the data through its own aligned buffer and handles the cache, so the
application never touches a channel, a descriptor or the cache. The
controllers themselves are never exposed.

Memory and storage
******************

External RAM
============

With ``APP_CAP_PSRAM``, :c:func:`app_svc_psram_map` allocates one region of
external RAM per application from the shared multi-heap external region,
rounded up to a power of two and at most
:kconfig:option:`CONFIG_APP_PLATFORM_PSRAM_MAX_BYTES`, zeroes it and maps it
into the application's domain, which then uses it directly with no call per
access. :c:func:`app_svc_psram_unmap`, a stop or a reclaim unmaps and frees it.

Files
=====

With ``APP_CAP_FS``, each application gets a private directory,
``<mount point>/apps/<application name>`` under
:kconfig:option:`CONFIG_APP_PLATFORM_FS_MOUNT_POINT`, created on first use.
:c:func:`app_svc_file_open`, :c:func:`app_svc_file_read`,
:c:func:`app_svc_file_write`, :c:func:`app_svc_file_seek`,
:c:func:`app_svc_file_close`, :c:func:`app_svc_file_delete` and
:c:func:`app_svc_file_size` take paths relative to it. An absolute path or a
``..`` component returns ``-EACCES``, so an application cannot leave its
directory. Handles come from a table of
:kconfig:option:`CONFIG_APP_PLATFORM_FS_OPEN_FILES` shared by all applications;
each records its owner, so one application cannot use another's handle.
Transfers are bounded by :kconfig:option:`CONFIG_APP_PLATFORM_FS_XFER_MAX`.
Files the application left open are closed when it stops; its files stay.

Networking
**********

The network services are described in :ref:`app_platform`; in short:

* ``APP_CAP_NET`` gates the link and MQTT. :c:func:`app_svc_net_ready` waits
  for the link, :c:func:`app_svc_wifi_status` reports it, and
  :c:func:`app_svc_wifi_connect` and :c:func:`app_svc_wifi_disconnect` change
  the device-wide Wi-Fi network. Each application has one MQTT session:
  :c:func:`app_svc_mqtt_open` sets its client id, publish topic and subscribe
  topic, :c:func:`app_svc_mqtt_publish` queues a message and
  :c:func:`app_svc_mqtt_recv` reads its inbox of four messages.
* ``APP_CAP_SOCKET`` gates :c:func:`app_svc_resolve` and one UDP or TCP socket
  per application, :c:func:`app_svc_udp_open`, :c:func:`app_svc_udp_send`,
  :c:func:`app_svc_udp_recv`, :c:func:`app_svc_udp_recv_from`,
  :c:func:`app_svc_udp_close`, :c:func:`app_svc_tcp_connect`,
  :c:func:`app_svc_tcp_send`, :c:func:`app_svc_tcp_recv` and
  :c:func:`app_svc_tcp_close`, with datagrams of up to
  :kconfig:option:`CONFIG_APP_PLATFORM_NET_DGRAM_MAX` bytes.
* Arriving data posts ``APP_EVENT_NET`` to a subscribed application. The
  session and the socket are closed when the application stops.

Bluetooth Low Energy
********************

Scan and heart rate
===================

The platform runs the Bluetooth stack on a thread of its own and serves one
request at a time for the whole device. With ``APP_CAP_BLE``,
:c:func:`app_svc_ble_scan` looks for a peer advertising a name, and
:c:func:`app_svc_ble_hr_read` connects to a peer advertising the Heart Rate
Service, reads one measurement and disconnects. The application never holds a
connection or an attribute handle.

GATT client
===========

With :kconfig:option:`CONFIG_APP_PLATFORM_BLE_GATT`,
:c:func:`app_svc_ble_gatt_read` and :c:func:`app_svc_ble_gatt_write` read and
write a characteristic of a named peer, given by service and characteristic
UUID. The service UUID must be one the firmware granted to the application with
:c:func:`app_platform_grant_ble_gatt`, up to
:kconfig:option:`CONFIG_APP_PLATFORM_BLE_GATT_MAX_SERVICES` per application,
or the call returns ``-EACCES``. The platform discovers the handles and keeps
one connection, owned by the application that used it last and reused for the
next request to the same peer; a request for another peer drops it first. When
the owner is reclaimed the connection is closed.

GATT server
===========

With :kconfig:option:`CONFIG_APP_PLATFORM_BLE_SERVE`, one application at a time
publishes a GATT service. :c:func:`app_svc_ble_serve_start` takes a granted
service UUID and up to four characteristics, each readable, writable or
notifiable. The platform builds and registers the attribute table, owns the
values, answers peer reads itself and advertises the device under the given
name. :c:func:`app_svc_ble_serve_set` and :c:func:`app_svc_ble_serve_get` set
and read a value, including one a peer wrote, :c:func:`app_svc_ble_notify`
updates a value and notifies a subscribed peer, and
:c:func:`app_svc_ble_advertise` and :c:func:`app_svc_ble_is_connected` control
advertising and report the link. The serving application receives
``APP_EVENT_GATT_WRITE`` with the characteristic index,
``APP_EVENT_BLE_CONNECTED`` and ``APP_EVENT_BLE_DISCONNECTED`` without
subscribing. :c:func:`app_svc_ble_serve_stop` stops advertising and removes the
service, and the platform does the same when the serving application is
reclaimed, so the next application finds the server free.

Application fetch
=================

With :kconfig:option:`CONFIG_APP_PLATFORM_BLE_OTA`,
:c:func:`app_svc_ble_ota_fetch` downloads a signed application from a peer
advertising its name over a GATT transfer service: first the manifest with its
signature, then the module, each checked against the limits of the loader. The
files are written as the staged copies of the registry; nothing is verified or
launched. The firmware verifies them with
:c:func:`app_loader_verify_load_manifest_file` and installs them with
:c:func:`app_registry_commit`, see :zephyr_file:`samples/subsys/app_ble_ota`.

Firmware interfaces
*******************

Registry
========

:kconfig:option:`CONFIG_APP_PLATFORM_REGISTRY` keeps the installed applications
in a file system on the ``storage_partition``, mounted by
:c:func:`app_registry_load` at
:kconfig:option:`CONFIG_APP_PLATFORM_REGISTRY_MOUNT_POINT`. The registry file
holds one record per application, with its name, version, autostart flag,
install order and module hash, and is replaced atomically on every change. Each
application has its module, ``<name>.llext``, and its signed manifest,
``<name>.manifest``, next to it, and a transfer writes the staged copies,
``<name>.llext.new`` and ``<name>.manifest.new``.

The staged install is: a transport writes the staged files, the firmware
verifies them with the loader, and :c:func:`app_registry_commit` moves them over
the installed ones and records the version, refusing a downgrade, or
:c:func:`app_registry_discard` deletes them. :c:func:`app_registry_count` and
:c:func:`app_registry_get` list the records, :c:func:`app_registry_uninstall`
removes an application and its files, and :c:func:`app_registry_autostart_all`
launches the applications marked for autostart at boot, in install order. See
:ref:`app_install_sample`, :ref:`app_fetch_sample` and
:zephyr:code-sample:`app-persist`.

Loader
======

The loader verifies a module before linking it, see :ref:`app_platform`.
:c:func:`app_loader_verify_load_manifest` takes a module in memory and
:c:func:`app_loader_verify_load_manifest_file` one in a file, each with its
signed manifest. :c:func:`app_loader_verify_load` checks a module against a
SHA-256 the firmware supplies. :c:func:`app_loader_heap_size`,
:c:func:`app_loader_manifest_caps` and :c:func:`app_loader_key` report what the
manifest declares and who signed it, :c:func:`app_loader_is_verified` tells
whether an extension came through a verifying loader, which the launch
requires, and :c:func:`app_loader_forget` drops an extension from the verified
set before it is unloaded.

.. _app_platform_shell:

Shell
=====

With :kconfig:option:`CONFIG_SHELL` the platform registers the ``app`` command
for operators. An ``<id>`` is a slot number below :c:func:`app_platform_count`.

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Command
     - Action
   * - ``app status``
     - Lists the installed applications with their version and autostart flag,
       when the registry is enabled, then every slot in use with its id, name,
       state (running, stopped or faulted), stack use, module size and share of
       CPU time, the total module memory and, with
       :kconfig:option:`CONFIG_SYS_HEAP_RUNTIME_STATS`, the system heap.
   * - ``app stop <id>``
     - Stops the application with :c:func:`app_platform_stop`, keeping its
       slot and module.
   * - ``app start <id>``
     - Starts a stopped application with :c:func:`app_platform_start`.
   * - ``app restart <id>``
     - Stops and starts it with :c:func:`app_platform_restart`.
   * - ``app remove <id>``
     - Stops it, unloads its module and frees the slot with
       :c:func:`app_platform_remove`.
   * - ``app log <on|off>``
     - Unmutes or mutes the output of :c:func:`app_svc_log` for every
       application with :c:func:`app_platform_log_enable`.

A firmware adds its own commands next to these, such as the ``store`` commands
of :ref:`app_install_sample` that install an uploaded module.
