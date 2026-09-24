# SPDX-License-Identifier: Apache-2.0
#
# Build a signed manifest that binds APP_NAME and APP_VERSION to MODULE_BIN,
# sign the manifest with an ECDSA P-256 development key, and emit, as comma-
# separated C byte literals, the manifest body (OUT_MF_INC), the signature
# (OUT_SIG_INC) and, when OUT_PUB_INC is set, the trusted public key in SEC1
# uncompressed form. The manifest carries the ABI identifier read from
# ABI_FILE, or ABI_OVERRIDE (32 hex digits) when set. A keypair is generated
# once at KEY_DIR and reused so the embedded public key stays stable across
# builds. The private key never leaves the build host.
#
# The manifest layout MUST match struct app_manifest (app_manifest.h):
#   uint32 magic ('A','M','F','2' LE) | uint32 version | uint32 module_len |
#   char name[32] | uint8 module_sha256[32] | uint8 abi_id[16]
#   -- packed, little-endian.
#
# Inputs: MODULE_BIN, KEY_DIR, APP_NAME, APP_VERSION, ABI_FILE, OUT_MF_INC,
#         OUT_SIG_INC, PYTHON_EXECUTABLE. Optional: OUT_PUB_INC, ABI_OVERRIDE.

if(DEFINED ABI_OVERRIDE)
  set(abi "${ABI_OVERRIDE}")
else()
  file(STRINGS "${ABI_FILE}" abi LIMIT_COUNT 1)
endif()

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
"import struct\nfrom cryptography.hazmat.primitives import hashes, serialization as s\nfrom cryptography.hazmat.primitives.asymmetric import ec, utils\npriv=s.load_pem_private_key(open(r'${priv}','rb').read(),None)\ndata=open(r'${MODULE_BIN}','rb').read()\nh=hashes.Hash(hashes.SHA256());h.update(data);mod=h.finalize()\nname=b'${APP_NAME}'\nassert len(name)<32,'name too long'\nname=name+b'\\x00'*(32-len(name))\nabi=bytes.fromhex('${abi}')\nassert len(abi)==16,'bad abi id'\nmf=struct.pack('<III',0x32464d41,${APP_VERSION},len(data))+name+mod+abi\nmh=hashes.Hash(hashes.SHA256());mh.update(mf);dig=mh.finalize()\nder=priv.sign(dig,ec.ECDSA(utils.Prehashed(hashes.SHA256())))\nr,ss=utils.decode_dss_signature(der)\nraw=r.to_bytes(32,'big')+ss.to_bytes(32,'big')\npub=priv.public_key().public_bytes(s.Encoding.X962,s.PublicFormat.UncompressedPoint)\ndef emit(b):\n    return ','.join('0x%02x'%x for x in b)\nopen(r'${OUT_MF_INC}','w').write(emit(mf)+'\\n')\nopen(r'${OUT_SIG_INC}','w').write(emit(raw)+'\\n')\nif r'${OUT_PUB_INC}':\n    open(r'${OUT_PUB_INC}','w').write(emit(pub)+'\\n')"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "failed to sign manifest")
endif()
