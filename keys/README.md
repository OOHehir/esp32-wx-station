# keys

App signing keys. **Never committed** — `.gitignore` covers this whole directory except
this file.

```sh
./tools/gen_signing_key.sh
```

| File | Scheme | Used by |
|---|---|---|
| `app_signing_rsa3072.pem` | Secure boot v2, RSA-3072 | esp32s3, esp32c3, esp32c6, & classic ESP32 built for rev v3.0+ |
| `app_signing_ecdsa256.pem` | Secure boot v1, ECDSA NIST P-256 | classic ESP32 below rev v3.0 |

Two files because the signing scheme follows the chip, not the project — the one place in
this template where a chip difference reaches something you have to manage by hand. Which
key a build uses is `CONFIG_SECURE_BOOT_SIGNING_KEY`, set per target in
`sdkconfig.defaults.<target>`.

## This key is the fleet

Signature verification without hardware secure boot takes its trusted key from the
signature block of the app **already running on the device**. Two things follow:

- **Lose the key & every deployed device is unreachable over OTA, permanently.** The
  only way back is a cable. Back it up somewhere that is not this working tree.
- **Rotating the key needs a bridge build**: an image signed with *both* the old & the
  new key, shipped first, so that devices running it will accept an image signed with the
  new one. Espressif's `espsecure sign-data --append-signatures` is how that is produced.

## Getting a device onto a signed image the first time

A device running an unsigned image has no trusted key at all, so it refuses every signed
update with *"No signatures were found for the running app"*. The first signed image has
to go over serial:

```sh
idf.py flash
```

After that OTA works normally, & every subsequent image is verified before it is
allowed to boot.
