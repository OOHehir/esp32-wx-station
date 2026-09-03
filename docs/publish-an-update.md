# Publish a firmware update

Tag a release, build it, put it on the server & watch a device take it. The device does the
rest: four independent checks, then a probation period it must survive before the update
becomes permanent.

**Before you start**

- A board already commissioned & reporting `"state": "connected"`
- The firmware server running & reachable — see [the server side](../nas/README.md)
- A clean working tree. `bump_version.sh` refuses a dirty one, for a reason given below

## 1. Tag the release

```bash
./tools/bump_version.sh minor
```

Where:

- `minor` may also be `major`, `patch`, or an explicit version such as `v1.2.3`

```
==> tagged v0.7.0 (was v0.6.1)
Not pushed. When you are ready:  git push origin v0.7.0
```

The version reaches the image from `git describe`, so a tree with uncommitted changes
produces `v0.7.0-dirty`. Update policy compares version strings for equality, so devices
would take that build & then never recognise the real tag as an update. Hence the refusal.

## 2. Build & publish

```bash
NAS_HOST=admin@nas.lan ./tools/publish.sh
```

Where:

- `NAS_HOST` is any target `rsync` accepts over ssh. Use `NAS_FIRMWARE_DIR` instead for a
  path already mounted locally
- `OTA_BASE_URL` overrides the URL written into the manifest; it must match
  `CONFIG_APP_OTA_BASE_URL` in the firmware, or devices will look where nothing was published

```
==> esp-ota v0.7.0 (esp32s3), 1028 KiB
    signed: yes
==> rsync -> admin@nas.lan:/volume1/docker/esp-ota/firmware
==> published
    manifest: https://nas.lan:8443/fw/esp-ota/esp32s3/manifest.json
    verified: the server returns this release (HTTP 200, v0.7.0)
```

That last line is the script fetching back what it just wrote & confirming the version
matches. Copying a file somewhere is not the same as publishing it: the first real run of
this script left the firmware directory readable only by the publishing user, & the server
answered 403 to everything while `rsync` reported success.

## 3. Watch a device take it

A device checks on its own every six hours, jittered. To see it now:

```bash
curl -X POST http://esp-ota-a1b2c3.local/api/ota/check
watch -n2 'curl -s http://esp-ota-a1b2c3.local/api/status | python3 -c "
import json,sys; o=json.load(sys.stdin)[\"ota\"]
print(o[\"state\"], o[\"progress\"], o[\"message\"])"'
```

```
checking  -1
downloading 42  v0.6.1 -> v0.7.0
ready      100  v0.7.0 installed, restarting
selftest    -1  on probation -- proving this image
idle        -1  v0.7.0 passed its self-test and is confirmed
```

`selftest` is the state worth understanding. The new image boots in `PENDING_VERIFY`, & the
bootloader will put the previous image back on the next reset unless the device confirms
it. Confirmation requires two minutes of uptime, a network, & a successful fetch of the
manifest over TLS. That last condition is the point: confirming an image is a promise that
a repair can still reach the device.

## Only a newer release installs itself

An automatic poll applies a manifest only when its version parses as `vX.Y.Z` and sorts
strictly above what the device is running. Anything else is refused & named on the status
page:

| Verdict | What it means |
|---|---|
| `not_newer` | the published version is older than the one running |
| `unorderable` | one side is not `vX.Y.Z` — `v0.2.0-3-gab18cdd`, `v0.2.0-dirty`, a bare `7b29c21` |

That is what makes it safe to publish a release & walk away from it. A stale artefact left
on the server cannot walk a fleet backwards months later, so nothing has to be gone back &
deleted.

**The cost, & it is a real one:** a board flashed over serial from an untagged tree reports
a version like `7b29c21`, which nothing can be proved newer than. It will never take an
automatic update. Tag before flashing a board you intend to update over the air.

## Roll back deliberately

A **forced** check — button B on the panel, the web UI's button, or the MQTT `check`
command — installs a version it cannot order, or one that is older. Automatic polls never
do. So moving a fleet backwards means publishing the older build & then asking each device
for it, rather than publishing & waiting.

The reason string says which happened, e.g. `v0.2.0 -> v0.1.0 is a downgrade -- installing
anyway, you asked`.

## If something goes wrong

**`manifest unavailable: ESP_ERR_NOT_FOUND`** — nothing is published at the path the device
dials, or the server cannot read it. Check both from the publishing host:

```bash
curl --cacert server_certs/ca_cert.pem https://nas.lan:8443/fw/esp-ota/esp32s3/manifest.json
```

A 403 rather than a 404 is a permissions problem, & the directory itself is the usual
culprit rather than the files inside it.

**`sha256 mismatch`** — the image on the server does not hash to what the manifest claims,
so the upload was truncated or corrupted. Publish again. Nothing was committed to the
passive slot: the check runs before the slot is made bootable.

**`image rejected: ESP_ERR_OTA_VALIDATE_FAILED`** — the signature did not verify. Either
the image was signed with a different key from the one that signed the firmware now
running, or it was not signed at all. A device on unsigned firmware has no trusted key &
refuses every signed update; the first signed image has to arrive over serial.

**`failed to run here 2 times -- not retrying automatically`** — the version was installed
twice & never survived its self-test, so automatic checks now skip it. `POST /api/ota/check`
retries it anyway, which is worth doing once if the cause was a bad upload rather than a
bad build.

**`this image is still on probation`, HTTP 409** — the running image has not confirmed
itself yet. ESP-IDF refuses to move the boot partition in that state, so the request is
refused before a megabyte is fetched to discover it. Wait for the self-test.

**The device reports the old version after a reboot** — the update was reverted. The status
message names it: `v0.7.0 was rolled back (failure #1)`. Read the serial log from the
probation boot; the self-test states which condition it could not meet.
