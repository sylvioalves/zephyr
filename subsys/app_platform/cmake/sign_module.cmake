# SPDX-License-Identifier: Apache-2.0
#
# Build a signed manifest that binds APP_NAME and APP_VERSION to MODULE_BIN,
# sign the manifest with an ECDSA P-256 development key, and emit, as comma-
# separated C byte literals, the manifest body (OUT_MF_INC), the signature
# (OUT_SIG_INC) and, when OUT_PUB_INC is set, the trusted public key in SEC1
# uncompressed form. When OUT_MANIFEST is set, the manifest followed by its
# signature is also written there as the binary a server hosts. A keypair is
# generated once at KEY_DIR and reused so the embedded public key stays stable
# across builds. The private key never leaves the build host.
#
# A module that imports a symbol outside the list named in ABI_FILE is not
# signed; SKIP_IMPORT_CHECK signs it anyway, for tests of the loader check.
#
# The manifest carries the interface major version and the other facts the
# firmware build wrote to ABI_FILE, the per-application heap size APP_HEAP_SIZE,
# the capabilities the application asks for, APP_REQUESTED_CAPS, and those it
# cannot run without, APP_REQUIRED_CAPS (none when unset). Each is a number or
# comma-separated capability names without the APP_CAP_ prefix, such as
# LOG,TIME. The application is granted no capability it does not request.
# The layout is built by sign_module.py and matches struct app_manifest
# (app_manifest.h). ABI_MAJOR_OVERRIDE, TARGET_OVERRIDE and MANIFEST_LEN
# produce a manifest this firmware refuses or one with fields it does not know,
# for tests.
#
# Inputs: MODULE_BIN, KEY_DIR, APP_NAME, APP_VERSION, APP_REQUESTED_CAPS,
#         ABI_FILE, OUT_MF_INC, OUT_SIG_INC, PYTHON_EXECUTABLE. Optional:
#         OUT_PUB_INC, OUT_MANIFEST, APP_HEAP_SIZE, APP_REQUIRED_CAPS,
#         ABI_MAJOR_OVERRIDE, TARGET_OVERRIDE, MANIFEST_LEN, SKIP_IMPORT_CHECK.

if(NOT DEFINED APP_REQUESTED_CAPS)
  message(FATAL_ERROR "APP_REQUESTED_CAPS is not set for ${APP_NAME}: list the "
    "capabilities the module uses, such as -DAPP_REQUESTED_CAPS=LOG,TIME")
endif()

set(args
  --module ${MODULE_BIN}
  --key ${KEY_DIR}/app_signing_key.pem
  --abi-file ${ABI_FILE}
  --name ${APP_NAME}
  --version ${APP_VERSION}
  --out-mf-inc ${OUT_MF_INC}
  --out-sig-inc ${OUT_SIG_INC}
)

foreach(opt
    OUT_PUB_INC:--out-pub-inc
    OUT_MANIFEST:--out-manifest
    APP_HEAP_SIZE:--heap-size
    APP_REQUESTED_CAPS:--requested-caps
    APP_REQUIRED_CAPS:--required-caps
    ABI_MAJOR_OVERRIDE:--abi-major
    TARGET_OVERRIDE:--target
    MANIFEST_LEN:--manifest-len)
  string(REPLACE ":" ";" opt ${opt})
  list(GET opt 0 var)
  list(GET opt 1 flag)
  if(DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
    list(APPEND args ${flag} ${${var}})
  endif()
endforeach()
if(SKIP_IMPORT_CHECK)
  list(APPEND args --skip-import-check)
endif()

execute_process(
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/sign_module.py ${args}
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "failed to sign ${MODULE_BIN}")
endif()
