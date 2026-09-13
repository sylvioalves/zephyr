.. zephyr:code-sample:: app-kv-store

   :name: Isolated key/value store application

   Store, read and delete keys from a user-mode application.

Overview
********

Launches two applications that use the platform's key/value store. The first
walks a key through its whole life: set it, read it back, overwrite it, delete
it, and confirm it is gone while a second key of its own survives. It then
reuses the freed slot, which is the reason a fixed-size store needs a delete at
all.

The second application checks that the store is namespaced by owner. It asks
for the same key name the first one wrote and is told the key does not exist,
cannot delete it, and can hold its own value under that name at the same time.
An application therefore names its keys without coordinating with any other.

Requirements
************

A board with userspace support.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/app_kv_store
   :board: esp32c5_devkitc/esp32c5/hpcore
   :goals: build flash
   :compact:

Each check prints ``PASS`` or ``FAIL`` with the value it observed, followed by
a count.

See :ref:`app_platform_policy` for why a resource is exposed scoped rather than
whole.
