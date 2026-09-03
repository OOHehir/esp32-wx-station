# server_certs

`ca_cert.pem` — the CA certificate the firmware embeds to validate the OTA server.

Not in git: it is generated for *your* NAS by `tools/gen_certs.sh`, & committing one
developer's CA would invite everyone else to pin the wrong one.

```sh
./tools/gen_certs.sh nas.lan 192.168.1.10
```

The firmware embeds the **CA** certificate rather than the server's leaf certificate, so
the NAS certificate can be re-issued without reflashing devices.
