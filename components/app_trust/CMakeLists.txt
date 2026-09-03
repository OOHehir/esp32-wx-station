# The CA certificate, not the leaf: the server's certificate can then be re-issued without
# reflashing every device (PLAN §3). EMBED_TXTFILES appends the NUL that mbedTLS's PEM
# parser requires.
#
# Exactly one component may embed a given file -- ESP-IDF names the generated assembly
# stub after the file's basename, and a second component embedding the same certificate
# makes ninja refuse to build with "multiple rules generate ca_cert.pem.S". That is the
# mechanical reason this component exists; PLAN §3 is the design reason.
#
# Conditional for the same reason SRCS may be: it is a build input, not a dependency. In
# bundle mode nothing is embedded, so a tree with no server_certs/ca_cert.pem still builds.
if(CONFIG_APP_TRUST_PRIVATE_CA)
    set(embed "${CMAKE_CURRENT_LIST_DIR}/../../server_certs/ca_cert.pem")
else()
    set(embed "")
endif()

idf_component_register(
    SRCS "app_trust.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls
    EMBED_TXTFILES ${embed})
