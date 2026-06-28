# SPDX-License-Identifier: Apache-2.0
#
# Sign MODULE_BIN with an ECDSA P-256 development key and emit, as comma-
# separated C byte literals, the raw signature (OUT_SIG_INC), the trusted
# public key in SEC1 uncompressed form (OUT_PUB_INC), and the module SHA-256
# (OUT_HASH_INC). A keypair is generated once at KEY_DIR and reused so the
# embedded public key stays stable across builds. The private key never leaves
# the build host; only the public key is compiled into the firmware.
#
# Inputs: MODULE_BIN, KEY_DIR, OUT_SIG_INC, OUT_PUB_INC, OUT_HASH_INC,
#         PYTHON_EXECUTABLE.

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

# Sign the module and dump signature(raw r||s) + pubkey(SEC1) + hash as hex.
execute_process(
  COMMAND ${PYTHON_EXECUTABLE} -c
"import sys\nfrom cryptography.hazmat.primitives import hashes, serialization as s\nfrom cryptography.hazmat.primitives.asymmetric import ec, utils\nfrom cryptography.exceptions import InvalidSignature\npriv=s.load_pem_private_key(open(r'${priv}','rb').read(),None)\ndata=open(r'${MODULE_BIN}','rb').read()\nh=hashes.Hash(hashes.SHA256());h.update(data);dig=h.finalize()\nder=priv.sign(dig,ec.ECDSA(utils.Prehashed(hashes.SHA256())))\nr,ss=utils.decode_dss_signature(der)\nraw=r.to_bytes(32,'big')+ss.to_bytes(32,'big')\npub=priv.public_key().public_bytes(s.Encoding.X962,s.PublicFormat.UncompressedPoint)\ndef emit(b):\n    return ','.join('0x%02x'%x for x in b)\nopen(r'${OUT_SIG_INC}','w').write(emit(raw)+'\\n')\nopen(r'${OUT_PUB_INC}','w').write(emit(pub)+'\\n')\nopen(r'${OUT_HASH_INC}','w').write(emit(dig)+'\\n')"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "failed to sign module")
endif()
