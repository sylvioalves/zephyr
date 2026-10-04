.. _app_platform_api:

API Reference
#############

The firmware side of the platform, used to launch and supervise applications:

.. doxygengroup:: app_platform

The capability bits:

.. doxygengroup:: app_platform_caps

The services, called by built-in applications as system calls:

.. doxygengroup:: app_platform_services

The interface a loadable module is built against, in ``app_sdk.h``:

.. doxygengroup:: app_platform_sdk

The per-application heap allocator:

.. doxygengroup:: app_platform_heap

The event dispatch helper in
:zephyr_file:`include/zephyr/app_platform/app_dispatch.h` has no group of its
own; it is described in :ref:`app_platform_services_catalog`.
