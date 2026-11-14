# OTA server on the Synology DS225+

An nginx container serving firmware images over TLS on port 8443. The DS225+ is a
Celeron J4125 (x86-64) with 2 GB RAM; `nginx:stable-alpine` uses roughly 10 MB, so
this is not a meaningful load on the NAS.

## One-time setup

### 1. Generate certificates (on your workstation, not the NAS)

```sh
./tools/gen_certs.sh nas.lan nas.lan
```

Pass **both** the hostname & the IP the device will dial — the name firmware
connects to must appear in the certificate's SAN list, & a mismatch shows up as a
TLS handshake failure that looks like a network fault.

Give the NAS a DHCP reservation first so the IP does not move.

`nas/certs/ca.key` is your CA private key: keep it on your workstation, never copy it
to the NAS. `server_certs/ca_cert.pem` is the public CA cert & gets embedded in
firmware.

### 2. Install Container Manager

Package Center → **Container Manager** → Install.

### 3. Copy the project to the NAS

Create `/volume1/docker/esp-ota` (File Station, or over ssh) & copy:

```
/volume1/docker/esp-ota/
├── docker-compose.yml
├── nginx.conf
├── certs/
│   ├── server.crt
│   └── server.key          # 0400 ideally; do NOT copy ca.key here
└── firmware/               # publish.sh writes into this
```

```sh
rsync -a nas/docker-compose.yml nas/nginx.conf admin@nas.lan:/volume1/docker/esp-ota/
rsync -a nas/certs/server.crt nas/certs/server.key admin@nas.lan:/volume1/docker/esp-ota/certs/
```

rsync only works once the DSM rsync service is on — see the gotcha below. Until then,
`scp -O` or tar-over-ssh will do the same job.

### 4. Bring the stack up

The Container Manager **Project** wizard rejects every compose file with *"the format of
docker-compose.yml is invalid"* on DSM 7.3.2 / Container Manager 24.0.2-1606. Treated as a
package defect, not a config error. Do not re-derive this — the following were each tested
and ruled out:

| Ruled out | How |
|---|---|
| Compose content | A minimal file (`image`, `ports`, `container_name`, `restart`) fails identically to the full one |
| Compose validity | `docker-compose config` on the NAS validates every variant, & `-q` returns clean |
| Transfer corruption | Local & NAS MD5s match |
| Encoding | Pure ASCII, LF endings, no BOM, trailing newline |
| Missing `version:` | Adding `version: "3.8"` / `'3.3'` changes nothing |
| Relative bind paths | Absolute `/volume1/...` paths change nothing |
| Missing bind sources | All four exist before the attempt |
| File permissions / Synology ACL | The compose file now carries the inherited ACL granting `user:ContainerManager` full access; still rejected |
| Backend health | `dockerd` running, `/volume1` 40% full, image pulls fine |
| CM itself | Container → Create works; only the Project path fails |
| CM's own compose dialect | A file matching `docker-compose.mustache` exactly — `version: '3'`, explicit `networks:` with `driver: bridge`, `/tcp` on ports — is still rejected |

Nothing is logged to `synoscgi.log`, `messages` or `ContainerManager.log` when it fails.
The UI string comes from `invalid_files_format` in
`/usr/syno/synoman/webman/3rdparty/ContainerManager/texts/enu/strings`, which several
backend error codes share, so the message itself identifies nothing.

Bring it up over ssh instead. The container still shows up in Container Manager under
**Containers**, just not as a Project:

```sh
ssh -t admin@nas.lan 'cd /volume1/docker/esp-ota && sudo /usr/local/bin/docker-compose up -d'
```

`sudo` is required — `/var/run/docker.sock` is `root:root` `0660` & `admin` is not
in a docker group.

### 5. Check what reaches the NAS

The DSM firewall is **disabled** on this NAS, & the boundary keeping the OTA server off
the internet is the router, not DSM. Nothing to do here as long as that holds — but it
means every LAN device can reach 8443, 22, & 873 (the rsync daemon, which enumerates
share names to anyone who asks but serves no data while *Enable rsync account* stays off).

Two things follow. Do not port-forward to this NAS; if you ever want remote OTA, put it
behind a VPN. And if you do switch the DSM firewall on later, allow TCP **8443** from your
LAN subnet or the fleet stops updating — that failure looks like a dead server, not a
blocked port.

## Verify before touching firmware

From your workstation:

```sh
curl --cacert nas/certs/ca.crt https://nas.lan:8443/healthz
curl --cacert nas/certs/ca.crt https://nas.lan:8443/fw/            # directory listing
```

If that returns `ok`, TLS & the certificate SANs are correct, & any later OTA
failure is a firmware-side problem. Debug the server here, not on the device.

Check the cipher & protocol the device will actually negotiate:

```sh
openssl s_client -connect nas.lan:8443 -CAfile nas/certs/ca.crt -tls1_2 </dev/null
```

## Publishing firmware

```sh
NAS_FIRMWARE_DIR=/media/dsCIFS/public/ota ./tools/publish.sh
```


`publish.sh` sets the modes it ships (`a+rX,go-w`) rather than letting the staging
directory's own permissions travel, & then fetches the manifest back over HTTPS to check
the server really will serve it. Both exist because the first real publish did neither.

`rsync -a src/ host:dest/` applies the **source directory's** mode to the destination
directory, & the script stages into `mktemp -d`, which is `0700`. So the first publish
left `/volume1/docker/esp-ota/firmware` readable only by the publishing user, & nginx —
a different user inside its container — answered **403 to everything**, including paths it
had served a minute earlier. rsync reported success. The device would have reported
`manifest unavailable` at its next poll, six hours later, with nothing linking the two.

If you ever see 403s from a server that was working, check the mode on the firmware
directory itself first, not just the files under it.

Reads the project name, version & target chip out of `build/project_description.json`,
copies the image & rewrites the manifest. Works for any target chip without
configuration.

Layout on the server:

```
public/ota/<project>/manifest.json
public/ota/<project>/<version>/<project>.bin
```

nginx serves the NAS's `public` share (`/volume1/public/ota`), so publishing is a copy onto
that share as mounted here over CIFS: `NAS_FIRMWARE_DIR=/media/dsCIFS/public/ota`. The
manifest names the chip, & a device refuses one written for another; the path has no chip
level because this project ships for the ESP32-C3 only.

## Notes & gotchas

- **Ports.** DSM owns 80, 443, 5000 & 5001. 8443 avoids them; if you change it,
  change it in both `docker-compose.yml` & `nginx.conf`.
- **DSM updates** restart containers. `restart: unless-stopped` handles it, but a major
  DSM upgrade can also re-enable the firewall — if the fleet stops updating after one,
  check that before anything else.
- **The container is read-only** (`read_only: true` plus tmpfs for nginx's cache and
  pid). The firmware directory is mounted read-only too; publishing happens over rsync,
  not through nginx.
- **Certificate expiry.** The server cert is 825 days, the CA 10 years. Re-issue the
  server cert any time with `gen_certs.sh` — because firmware pins the *CA*, no reflash
  is needed. Do not let the CA lapse: recovering from that needs a wired reflash.
- **DSM gates rsync behind a service toggle.** `/usr/bin/rsync` on DSM is a setuid
  Synology build that refuses `--server` mode unless Control Panel → File Services →
  **rsync** → *Enable rsync service* is ticked. This applies to rsync over ssh, not just
  the rsync daemon, & ssh key auth succeeding tells you nothing about it. The failure
  surfaces client-side as `Permission denied, please try again.` followed by
  `connection unexpectedly closed`, which reads like an auth problem & is not one — run
  `ssh admin@nas.lan rsync --server -nlogDtpre.iLsfxCIvu . /tmp/` to see the real message,
  `rsync service is no running (code 43)`.
- **scp needs `-O`.** DSM ships sshd without the sftp subsystem, so OpenSSH 9's
  sftp-based scp fails with `subsystem request failed on channel 0`. `scp -O` uses the
  legacy protocol & works. `tar cf - -C nas . | ssh admin@nas.lan 'tar xf - -C <dir>'`
  also works & depends on neither service.
- **A bad nginx.conf looks like a network fault.** nginx validates its config at
  startup & exits if it fails, so the container never binds 8443 & `curl` reports
  `Failed to connect` — with `restart: unless-stopped` it then crash-loops quietly. Check
  `sudo docker logs esp-ota-nginx` before suspecting TLS, the firewall or the cert. The
  tell that the container is down rather than blocked: a firewalled port hangs, a
  container that never started refuses in milliseconds.
- **`limit_except` is location-only.** Putting it directly in a `server` block gives
  `[emerg] "limit_except" directive is not allowed here`. To bar writes server-wide, use
  `if ($request_method !~ ^(GET|HEAD)$) { return 405; }` instead — `return` inside `if`
  is one of the documented-safe uses, & it covers locations added later.
- **`can not modify /etc/nginx/conf.d/default.conf (read-only file system?)`** in the
  container log is `info`, not an error. It is the stock entrypoint failing to add an
  IPv6 listener to a file this setup overrides anyway. Ignore it.
- **Image pinning.** `nginx:stable-alpine` follows upstream. Pin a digest if you want
  the server to be reproducible.

---

## Mosquitto (optional, PLAN §8)

A second container in the same stack, for pushing **"check for an update now"** to a whole
fleet instead of waiting out each device's jittered six-hour poll. Nothing in the OTA path
depends on it, & firmware ships with MQTT compiled out — bring it up only if you want it.

```sh
./tools/gen_mqtt_user.sh                 # prints the password; save it, it is hashed on disk
rsync -a nas/mosquitto.conf nas/mosquitto/passwd admin@nas.lan:/volume1/docker/esp-ota/
ssh -t admin@nas.lan 'cd /volume1/docker/esp-ota && sudo /usr/local/bin/docker-compose up -d'
```

Then put the username & password into `CONFIG_APP_MQTT_USERNAME` / `CONFIG_APP_MQTT_PASSWORD`
and set `CONFIG_APP_MQTT_ENABLE=y`.

It reuses **the same certificate & CA as nginx**, so there is one trust anchor for the
whole stack & the device needs nothing extra embedded. TLS on 8883 & no plaintext
listener at all: a 1883 port gets used "just for testing" & then stays.

```sh
mosquitto_sub -h nas.lan -p 8883 --cafile nas/certs/ca.crt -u esp-ota -P '...' -t 'esp-ota/#' -v
mosquitto_pub -h nas.lan -p 8883 --cafile nas/certs/ca.crt -u esp-ota -P '...' \
              -t esp-ota/all/cmd -m check
```

Devices publish retained status & a retained `online` flag with a last will, so a
dashboard that connects later still sees the fleet — including which of them are gone.

**The password is shared by every device on the broker.** Anyone who can read an image can
read it. That is acceptable because what the broker controls is "check now" & the update
itself is signed & verified independently; if MQTT ever carries more than that, move to
per-device client certificates. `mosquitto.conf` says where.
