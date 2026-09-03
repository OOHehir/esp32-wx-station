# Fail the configure step, with the command that fixes it, when the CA certificate the
# firmware is supposed to embed is not there.
#
# It is not in git and it never should be -- committing one developer's CA invites
# everyone else to pin the wrong one -- so a fresh clone does not have it, and the error
# CMake produces on its own is "Cannot find source file: .../server_certs/ca_cert.pem"
# at *generate* time, which names a PEM as a source file and suggests nothing. This runs
# first and says what to do.
#
# Same reasoning as cmake/check_partitions.cmake and cmake/check_signing_key.cmake: a
# guard that only fires on a fresh clone is a guard nobody is watching, which is exactly
# why it has to be loud.

if(NOT CONFIG_APP_TRUST_PRIVATE_CA)
    # Trusting the public root bundle instead -- nothing is embedded (PLAN §3).
    return()
endif()

set(_ca "${CMAKE_CURRENT_LIST_DIR}/../server_certs/ca_cert.pem")
if(EXISTS "${_ca}")
    message(STATUS "check_server_cert: embedding server_certs/ca_cert.pem")
    return()
endif()

message(FATAL_ERROR
    "The OTA server's CA certificate is missing:\n"
    "    server_certs/ca_cert.pem\n\n"
    "Generate your own CA and server certificate, naming every hostname and IP the "
    "device might dial:\n"
    "    ./tools/gen_certs.sh <hostname> <ip>\n"
    "    e.g. ./tools/gen_certs.sh nas.lan 192.168.1.10\n\n"
    "It is deliberately not in git: the device trusts whatever CA is embedded here, so "
    "shipping one developer's CA would have everyone else pinning the wrong one. To "
    "trust the public root CAs instead -- for a server with a certificate from a public "
    "issuer -- set CONFIG_APP_TRUST_CERT_BUNDLE=y, which embeds nothing.")
