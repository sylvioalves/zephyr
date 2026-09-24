# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_install)

# Identity bound into the manifest of the bundled reference app. Uploaded apps
# are signed on the host with the same key and verified against the public key
# embedded from this build.
set(APP_NAME    "uploaded")
set(APP_VERSION 1)

target_sources(app PRIVATE src/main.c)

set(ext_src ${PROJECT_SOURCE_DIR}/module/src/install_app.c)
set(bin ${PROJECT_BINARY_DIR}/llext/install_app.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/install_app.inc)
set(mf  ${ZEPHYR_BINARY_DIR}/include/generated/install_app.manifest.inc)
set(sig ${ZEPHYR_BINARY_DIR}/include/generated/install_app.sig.inc)
set(pub ${ZEPHYR_BINARY_DIR}/include/generated/install_app.pubkey.inc)

add_llext_target(install_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(install_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${bin} ${inc})

# Sign a manifest binding the name, version and module hash; embed the
# manifest, signature and trusted public key. The same key signs apps uploaded
# from the host. The private key is generated once and never compiled in.
add_custom_command(
	OUTPUT  ${mf} ${sig} ${pub}
	COMMAND ${CMAKE_COMMAND}
		-DMODULE_BIN=${bin}
		-DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
		-DAPP_NAME=${APP_NAME}
		-DAPP_VERSION=${APP_VERSION}
		-DABI_FILE=${APP_PLATFORM_ABI_FILE}
		-DOUT_MF_INC=${mf}
		-DOUT_SIG_INC=${sig}
		-DOUT_PUB_INC=${pub}
		-DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
		-P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
	DEPENDS ${bin} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(install_sign DEPENDS ${mf} ${sig} ${pub})
add_dependencies(install_sign app_platform_abi)
add_dependencies(app install_sign)
