# Command a fleet

A device checks for updates on its own jittered six-hour poll. That is correct & unhurried,
& no help at all when a fix has just been published & the question is whether it lands.
An MQTT broker answers that question for every device at once.

Optional in the strong sense: it is off by default, & nothing in the update path depends on
it. A broker that goes down means the fleet stops reporting, not that a device is stuck.

**Before you start**

- A broker reachable from the devices. [`nas/README.md`](../nas/README.md) documents adding
  'Mosquitto' (an MQTT broker) to the same container stack as the firmware server
- `mosquitto_pub` & `mosquitto_sub` on the machine you drive it from
  (`apt install mosquitto-clients`)
- A username & password the broker accepts

## 1. Create a broker user

```bash
./tools/gen_mqtt_user.sh
```

```
==> wrote nas/mosquitto/passwd for user 'esp-ota'
    username: esp-ota
    password: <24 random characters, printed once>
```

> **NOTE:** the password is hashed in that file & is not recoverable from it. Save it now.

## 2. Put the credentials in the firmware

They belong in 'sdkconfig.local' (gitignored), not 'sdkconfig.defaults' (tracked). The
password is compiled into the image & shared by the whole fleet; committing it would carry
one developer's credential into every copy of the template.

```ini
CONFIG_APP_MQTT_ENABLE=y
CONFIG_APP_MQTT_BROKER_URI="mqtts://nas.lan:8883"
CONFIG_APP_MQTT_USERNAME="esp-ota"
CONFIG_APP_MQTT_PASSWORD="<the password printed above>"
```

Where:

- `mqtts://` uses TLS & the same certificate authority the update path trusts, so there is
  no second anchor to manage. The host must appear in the certificate's Subject Alternative
  Name list
- `mqtt://` also works, for a broker you already run. The firmware warns at boot, because
  the username, password & every command then cross the network in the clear

Rebuild & publish as usual — see [Publish an update](publish-an-update.md).

## 3. Watch the fleet

```bash
mosquitto_sub -h nas.lan -p 8883 --cafile nas/certs/ca.crt \
              -u esp-ota -P '<the password printed above>' -t 'esp-ota/#' -v
```

Where:

- `--cafile` is the certificate authority created by `gen_certs.sh`
- `esp-ota/#` subscribes to every topic under the prefix; `#` is the MQTT multi-level
  wildcard

```
esp-ota/A1B2C3/online 1
esp-ota/A1B2C3/status {"device":"esp-ota-a1b2c3","version":"v0.7.0","partition":"ota_0",
                      "uptime_s":62,"heap_free":229488,"rssi":-52,"ota_state":"idle",
                      "probation":false,"crash_dump":false}
```

Both are retained, so a dashboard that connects later still sees the fleet. `online` carries
a last will of `0`, published by the broker when a device stops answering — without it a
board that lost power stays "online" for ever, because the last thing it said was that it
was fine.

## 4. Push a check to every device

```bash
mosquitto_pub -h nas.lan -p 8883 --cafile nas/certs/ca.crt \
              -u esp-ota -P '<the password printed above>' \
              -t esp-ota/all/cmd -m check
```

| Topic | Reaches |
|---|---|
| `esp-ota/<id>/cmd` | one device, by the identifier in its status document |
| `esp-ota/all/cmd` | every device on this prefix |

| Payload | Effect |
|---|---|
| `check` | check for a firmware update now |
| `restart` (or `reboot`) | restart, unless an image is being written |
| `led-on`, `led-off` | hold the status LED |
| `led-auto` | release the hold, back to the status pattern |
| `status` | publish the status document immediately |

Payloads are bare words rather than JSON: `-m check` is something you can type from memory,
& a structured envelope on a command topic has to earn its complexity.

A pushed `check` deliberately does **not** override the quarantine on a version that has
already failed twice on that device. A broadcast is automation, not a person who has decided
a particular version deserves another attempt; one broken release plus one broadcast should
not re-download it onto every device that already rejected it.

## Testing without a broker

`test/rig/broker.py` speaks just enough MQTT to exercise the firmware, using a certificate
minted from the same certificate authority:

```bash
./test/rig/run.sh          # mints the certificate, if not already done
./test/rig/run_broker.sh
```

It is not a broker & should not outlive the test — no QoS 2, no persistence, no wildcards.

## If something goes wrong

**Nothing appears under the topic prefix** — the device is not connecting. The serial log
names the reason: `transport error` is TLS or the network, `connect refused, code 4` is the
username or password, & `code 5` is an access-control rule on the broker.

**`SSL related configs set, but the URI scheme specifies a non-SSL scheme`** — an `mqtt://`
URI in a build expecting TLS. Fixed in current firmware, which applies the trust anchor only
for `mqtts://`.

**Commands are ignored with nothing in the log** — an unrecognised payload does nothing &
answers nothing, deliberately: a broker carries other people's traffic. Check spelling
against the table above; matching is case-insensitive & surrounding whitespace is tolerated.

**`check` returns nothing on one device** — it may be on probation after an update, when
another cannot be installed. Its status document reports `"probation": true`.

## See also

[The server side](../nas/README.md) · [Publish an update](publish-an-update.md)
