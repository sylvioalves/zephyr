.. _app_platform_getting_started:

Getting Started
###############

This page is a quick tour of the application platform: what it is made of, how
an application moves from source code to a running, supervised group, and the
smallest built-in application and loadable module that run on it. The concepts
behind each step are described in :ref:`app_platform`, and the services an
application can call are listed in :ref:`app_platform_services_catalog`.

What the platform is
********************

The platform turns a Zephyr firmware into a host for applications. The firmware
keeps the drivers, the network stack and the rest of the system. An application
is a unit of code with a name, a set of capabilities and, optionally, a heap,
which the platform runs in a slot of its own:

* Each slot has its own thread stack and, with :kconfig:option:`CONFIG_USERSPACE`,
  its own memory domain, so an application reaches only its own memory.
* An application reaches the system only through the platform services, the
  ``app_svc_*`` calls. Each call checks the capability mask of the calling
  application and the resources granted to it before it does any work.
* A supervisor watches every application. A fault, a hang or a return from the
  entry point reclaims the application: its threads end, the resources it held
  are released, and the outputs it drove return to idle. The rest of the system
  keeps running.

An application is either built into the firmware image, as a C function, or
loaded at runtime as a signed :ref:`linkable loadable extension <llext>`, a
module that can be installed and updated without rebuilding the firmware.

Architecture
************

.. graphviz::
   :caption: Application platform architecture

   digraph app_platform {
       rankdir=TB;
       node [shape=box, style=rounded, fontsize=10];
       edge [fontsize=9];

       subgraph cluster_apps {
           label="Applications (one slot each, user mode)";
           style=dashed;
           app_a [label="Built-in application\nentry function"];
           app_b [label="Loadable module\napp_main()"];
           app_c [label="Worker threads\nof an application"];
       }

       subgraph cluster_platform {
           label="Platform (kernel mode)";
           services [label="Service library\napp_svc_*: capability check,\ngrant check, per-application state"];
           supervisor [label="Supervisor\nlaunch, fault and hang handling,\nrestart budget"];
           reaper [label="Reaper\nreclaim hooks, module unload"];
           loader [label="Loader\nsignature, hash, target,\ninterface, imports"];
           registry [label="Registry\ninstalled applications,\nautostart"];
           wdt [label="Task watchdog\nmonitored applications"];
       }

       drivers [label="Zephyr drivers and subsystems\nGPIO, I2C, SPI, BLE, networking, file system, ..."];

       app_a -> services [label="system call"];
       app_b -> services [label="exported\nfunction"];
       app_c -> services;
       services -> drivers;
       loader -> app_b [label="verified\nextension"];
       registry -> loader [label="stored module\nand manifest"];
       supervisor -> app_a [label="launch"];
       supervisor -> app_b;
       app_a -> supervisor [label="fault or exit", style=dashed];
       wdt -> supervisor [label="hang", style=dashed];
       supervisor -> reaper [label="reclaim"];
       reaper -> services [label="release grants\nand outputs", style=dashed];
   }

The parts of the platform are:

Service library
   The ``app_svc_*`` calls, enabled with
   :kconfig:option:`CONFIG_APP_PLATFORM_SERVICES` and one option per service. A
   built-in application reaches them as system calls; a loadable module reaches
   them through versioned functions the firmware exports, see
   :ref:`app_platform_abi_stability`. Each service checks the capability first,
   then the grant, and keeps its state per application slot.

Supervisor
   A thread at :kconfig:option:`CONFIG_APP_PLATFORM_SUPERVISOR_PRIORITY`, above
   every application, that launches applications and acts on reclaim requests. A small death
   watcher per slot waits for the application thread to end, so a crash or a
   return is noticed without the application's cooperation.

Reaper
   A thread at :kconfig:option:`CONFIG_APP_PLATFORM_REAPER_PRIORITY` that runs
   the reclaim hooks of the services and unloads the module of a removed
   application, so the supervisor never blocks on driver or service cleanup.

Loader and registry
   The loader, :kconfig:option:`CONFIG_APP_PLATFORM_LOADER`, accepts a module
   only after its signed manifest and its bytes check out. The registry,
   :kconfig:option:`CONFIG_APP_PLATFORM_REGISTRY`, records which applications
   are installed, their versions and which start at boot.

Task watchdog
   Applications that opt into hang detection get a channel of the
   :ref:`task watchdog <task_wdt_api>`, see :ref:`app_platform_liveness`.

Application lifecycle
*********************

.. graphviz::
   :caption: Lifecycle of an application slot

   digraph lifecycle {
       rankdir=LR;
       node [shape=box, style=rounded, fontsize=10];
       edge [fontsize=9];

       launched [label="Launched\nslot, domain, heap"];
       stopped [label="Stopped\nheld by the firmware"];
       running [label="Running"];
       reclaimed [label="Reclaimed\ngrants dropped,\noutputs idle"];
       free [label="Slot free"];

       launched -> running [label="configure hook"];
       launched -> stopped [label="stopped = true"];
       stopped -> running [label="app_platform_start()"];
       running -> reclaimed [label="fault, hang,\nexit, stop"];
       reclaimed -> running [label="resilient:\nrelaunch"];
       reclaimed -> stopped [label="app_platform_stop()"];
       reclaimed -> free [label="app_platform_remove()\nor exit"];
   }

A built-in application is described by a :c:struct:`app_def` and launched with
:c:func:`app_platform_launch`. A loadable module goes through more steps before
it runs:

#. **Build.** The module is compiled with ``app_platform_add_module()``, which
   takes the arguments of ``add_llext_target()``, against
   :zephyr_file:`include/zephyr/app_platform/app_sdk.h` only. Outside the tree,
   the module development kit builds the same module, see
   :ref:`app_platform`.
#. **Sign.** ``sign_module.cmake`` writes a manifest that binds the
   application name, version, heap size, requested and required capabilities,
   the module length and SHA-256, the interface major version and the target
   fingerprint, and signs it with an ECDSA P-256 key the firmware trusts.
#. **Transfer and install.** The module and its manifest reach the device
   storage, for example with ``west app install`` over the serial shell, with
   the BLE fetch service or over the network, and are staged next to the
   installed copy.
#. **Verify and load.** :c:func:`app_loader_verify_load_manifest_file` checks
   the signature with the key the manifest names, the name prefix of that key,
   the module length and hash, the target fingerprint, the interface major
   version, the ELF flags and the symbols the module imports, and only then
   links the module.
#. **Commit.** :c:func:`app_registry_commit` moves the staged files over the
   installed ones and records the version, refusing a downgrade.
#. **Launch.** :c:func:`app_platform_launch_llext` or
   :c:func:`app_platform_launch_llext_opts` takes a slot, maps the module and
   the heap into the slot's memory domain, computes the capabilities the
   application holds from the manifest, the launcher ceiling and the key
   ceiling, runs the configure hook and starts the entry thread.
#. **Run.** The application calls the services. Its worker threads, if any,
   share its domain, capabilities and grants.
#. **Reclaim.** A fault, a missed heartbeat, a return from ``app_main()``, a
   stop request or a removal ends the application. The reaper drops its
   grants, runs every service's reclaim hook and, on removal, unloads the
   module. A resilient built-in application is relaunched after a fault, up to
   :kconfig:option:`CONFIG_APP_PLATFORM_MAX_RESTARTS` times.

At boot, :c:func:`app_registry_autostart_all` repeats the verify and launch
steps for every installed application marked for autostart.

A first built-in application
****************************

A built-in application is a function the firmware launches as a group. This
example is taken from the :zephyr:code-sample:`app-kv-store` sample: the
application holds the logging and key/value capabilities and keeps a value in
its own key space.

.. code-block:: cfg
   :caption: prj.conf

   CONFIG_APPLICATION_DEFINED_SYSCALL=y

   CONFIG_APP_PLATFORM=y
   CONFIG_APP_PLATFORM_MAX_APPS=4
   CONFIG_APP_PLATFORM_SERVICES=y

   CONFIG_TASK_WDT=y

.. code-block:: c
   :caption: src/main.c

   #include <zephyr/app_platform/app_platform.h>
   #include <zephyr/app_platform/app_services.h>
   #include <zephyr/app_platform/app_caps.h>
   #include <zephyr/kernel.h>
   #include <zephyr/sys/printk.h>

   static void owner_app(void *arg)
   {
           uint32_t v = 0;

           ARG_UNUSED(arg);

           (void)app_svc_kv_set("count", 41);
           if (app_svc_kv_get("count", &v) == 0 && v == 41U) {
                   app_svc_log("kv_store: read it back");
           }
   }

   int main(void)
   {
           const struct app_def owner = {
                   .name = "kvowner",
                   .source = APP_BUILTIN,
                   .entry = owner_app,
                   .caps = APP_CAP_LOG | APP_CAP_KV,
           };

           if (app_platform_init() != 0) {
                   printk("platform init failed\n");
                   return 0;
           }

           int id = app_platform_launch(&owner);

           if (id < 0) {
                   printk("launch failed (%d)\n", id);
           }
           return 0;
   }

The application runs in user mode: it holds no kernel object and calls nothing
but the services. Its key space is keyed by its slot, so a second application
using the same key name sees its own value. An application that needs memory
the firmware shares with it, such as a result buffer, is launched with
:c:member:`app_def.stopped` set, gets the partition added to
:c:func:`app_platform_domain`, and is then started with
:c:func:`app_platform_start`, as the sample does.

A first loadable module
***********************

A module includes only ``app_sdk.h`` and exports an ``app_main()`` entry point.
This module is the one the :zephyr:code-sample:`app_edk` sample builds:

.. code-block:: c
   :caption: hello.c

   #include <stdio.h>
   #include <zephyr/app_platform/app_sdk.h>

   void app_main(void *arg)
   {
           char msg[64];

           ARG_UNUSED(arg);

           for (int i = 1; i <= 3; i++) {
                   snprintf(msg, sizeof(msg), "hello: built outside the tree, run %d", i);
                   app_svc_log(msg);
                   app_sleep_ms(100);
           }
   }
   EXPORT_SYMBOL(app_main);

Inside the firmware tree, the firmware build compiles and signs the module. The
:ref:`app_install_sample` sample does it this way (shortened):

.. code-block:: cmake
   :caption: CMakeLists.txt

   set(APP_NAME    "uploaded")
   set(APP_VERSION 1)
   set(APP_REQUESTED_CAPS LOG,RANDOM)
   set(APP_REQUIRED_CAPS  LOG)

   set(ext_src ${PROJECT_SOURCE_DIR}/module/src/install_app.c)
   set(bin ${PROJECT_BINARY_DIR}/llext/install_app.llext)
   set(mf  ${ZEPHYR_BINARY_DIR}/include/generated/install_app.manifest.inc)
   set(sig ${ZEPHYR_BINARY_DIR}/include/generated/install_app.sig.inc)
   set(bundle_dir ${PROJECT_BINARY_DIR}/bundle)
   set(bundle_ext ${bundle_dir}/${APP_NAME}.llext)
   set(bundle_mf  ${bundle_dir}/${APP_NAME}.manifest)

   app_platform_add_module(install_ext OUTPUT ${bin} SOURCES ${ext_src})

   add_custom_command(
     OUTPUT  ${mf} ${sig} ${bundle_ext} ${bundle_mf}
     COMMAND ${CMAKE_COMMAND}
       -DMODULE_BIN=${bin}
       -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
       -DAPP_NAME=${APP_NAME}
       -DAPP_VERSION=${APP_VERSION}
       -DAPP_REQUESTED_CAPS=${APP_REQUESTED_CAPS}
       -DAPP_REQUIRED_CAPS=${APP_REQUIRED_CAPS}
       -DABI_FILE=${APP_PLATFORM_ABI_FILE}
       -DOUT_MF_INC=${mf}
       -DOUT_SIG_INC=${sig}
       -DOUT_MANIFEST=${bundle_mf}
       -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
       -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
     DEPENDS ${bin} ${APP_PLATFORM_ABI_FILE}
   )

The firmware enables the loader and the registry, and installs an uploaded
module from the shell. The heart of its install command verifies the staged
module, commits it and launches it within a capability ceiling:

.. code-block:: c
   :caption: src/main.c (excerpt)

   #define APP_CAPS_CEILING (APP_CAP_LOG | APP_CAP_TIME | APP_CAP_RANDOM)

   rc = app_loader_verify_load_manifest_file(mf, mf_len, sig, APP_MANIFEST_SIG_LEN, path,
                                             "app_main", &ext, &entry);
   if (rc != 0) {
           app_registry_discard(name);
           return rc;
   }

   rc = app_registry_commit(name, true, mf);
   if (rc != 0) {
           app_loader_forget(ext);
           llext_unload(&ext);
           return rc;
   }

   rc = app_platform_launch_llext(mf->name, ext, (app_entry_t)entry, NULL,
                                  APP_CAPS_CEILING);

The module requests ``LOG`` and ``RANDOM``, and the ceiling allows both, so
the application holds exactly those two capabilities. A host installs a new
version of it with:

.. code-block:: console

   west app install -d build -p /dev/ttyUSB0

Where to go next
****************

* :ref:`app_platform` - isolation, capabilities and grants, supervision,
  loading, signing keys and the module interface.
* :ref:`app_platform_services_catalog` - every service, its capability, its
  grant and how it behaves.
* :ref:`app_platform_targets` - the boards and SoCs the platform runs on.
* :ref:`app_platform_policy` - how a Zephyr subsystem is classified before an
  application may reach it.
