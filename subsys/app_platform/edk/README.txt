Application module development kit
===================================

This kit builds, checks and signs loadable application modules for one
firmware, without the Zephyr tree and without the firmware sources. A module
built with it loads on that firmware and on any later build of it with the same
interface major version (abi_major) and target fingerprint (target), both in
firmware.txt.

Contents
--------

firmware.txt       What the signer binds a module to: interface major version,
                   target fingerprint and the description it is hashed from,
                   board, architecture, toolchain and compiler version.
keys.json          Public part of the firmware table of trusted signing keys:
                   key ids, states, capability ceilings, name prefixes.
abi/app_abi.lock   The interface the firmware exports to modules.
abi/app_imports.txt
                   The only symbols a module may import.
include/           The module SDK header <zephyr/app_platform/app_sdk.h> and
                   the headers it includes. Kernel and syscall headers are not
                   part of the kit.
cmake/             app_edk.cmake, the CMake helper; toolchain.cmake; the
                   compiler flags of the firmware target.
scripts/           The signer and its Python requirements.

Requirements
------------

- CMake 3.20 or later and a build tool such as Ninja or make.
- The toolchain named in firmware.txt (toolchain_variant, triple,
  compiler_version, sdk_version); the kit does not hold it.
- Python 3.9 or later with the packages in scripts/requirements.txt:

    pip install -r scripts/requirements.txt

Building a module
-----------------

A module includes only <zephyr/app_platform/app_sdk.h> (and the C library
headers of the toolchain), defines app_main() and exports it with
EXPORT_SYMBOL(app_main). Its CMakeLists.txt:

    cmake_minimum_required(VERSION 3.20.0)
    include(${APP_EDK_DIR}/cmake/app_edk.cmake)
    project(hello LANGUAGES C)
    app_edk_add_module(hello NAME hello VERSION 1 SOURCES hello.c
                       REQUESTED_CAPS LOG,TIME)

Configure it with the kit, the toolchain and the signing key:

    cmake -S . -B build -G Ninja -DAPP_EDK_DIR=<kit> \
          -DAPP_EDK_CROSS_COMPILE=<sdk>/gnu/<triple>/bin/<triple>- \
          -DAPP_SIGNING_KEY=<private key.pem> -DAPP_SIGNING_KEY_ID=<id>
    cmake --build build

Instead of APP_EDK_CROSS_COMPILE, ZEPHYR_SDK_INSTALL_DIR may name the Zephyr
SDK. The build writes build/hello.llext and build/hello.manifest, the signed
manifest followed by its signature: the bundle the firmware installs.

The build fails when a source includes a kernel or syscall header, when the
module imports a symbol outside abi/app_imports.txt, and when the key is not
the one the firmware lists under the key id, is revoked, may not sign the
application name or may not grant a required capability. Key id 0 is the
development key; a firmware accepts it only when it was built with it, and the
writer then needs that private key.

Compatibility
-------------

The firmware refuses a module whose manifest names another interface major
version or another target fingerprint, one importing a symbol it does not
export, and one signed by a key it does not list. A firmware rebuilt with other
options, drivers, syscalls or tick rate keeps accepting the module as long as
the interface major version, the target fingerprint and the signing key stay.
Building the firmware for another SoC or with other code generation options
changes the fingerprint; an incompatible interface change raises the major
version. Both need a new kit and a rebuilt module.
