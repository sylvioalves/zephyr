# SPDX-License-Identifier: Apache-2.0
#
# Emit only the trusted public key, as comma-separated C byte literals, from
# the development keypair at KEY_DIR (generating it once if absent).
#
# The device side needs nothing else: it never signs, and the image it verifies
# is built and signed by the peer. Deriving the key here rather than signing a
# local copy of the module is what keeps the two sides in agreement, since the
# same source compiled for two different boards does not produce the same bytes.
#
# Inputs: KEY_DIR, OUT_PUB_INC, PYTHON_EXECUTABLE.

set(priv "${KEY_DIR}/app_signing_key.pem")

if(NOT EXISTS "${priv}")
  file(MAKE_DIRECTORY "${KEY_DIR}")
  execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c
      "from cryptography.hazmat.primitives.asymmetric import ec\nfrom cryptography.hazmat.primitives import serialization as s\nk=ec.generate_private_key(ec.SECP256R1())\nopen(r'${priv}','wb').write(k.private_bytes(s.Encoding.PEM,s.PrivateFormat.PKCS8,s.NoEncryption()))"
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "failed to generate signing key")
  endif()
endif()

execute_process(
  COMMAND ${PYTHON_EXECUTABLE} -c
"from cryptography.hazmat.primitives import serialization as s\npriv=s.load_pem_private_key(open(r'${priv}','rb').read(),None)\npub=priv.public_key().public_bytes(s.Encoding.X962,s.PublicFormat.UncompressedPoint)\nopen(r'${OUT_PUB_INC}','w').write(','.join('0x%02x'%x for x in pub)+'\\n')"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "failed to emit public key")
endif()
