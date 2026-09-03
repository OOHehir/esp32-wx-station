#!/usr/bin/env python3
"""A minimal MQTT 3.1.1 broker, for proving app_mqtt on a bench.

The same idea as serve_firmware.py: a stand-in for the real thing, using a certificate
minted from the project CA so the device trusts it exactly as it would trust Mosquitto on
the NAS. It speaks just enough of MQTT to exercise the firmware -- CONNECT with a username
and password, SUBSCRIBE, PUBLISH in both directions, PINGREQ, and the last will on an
unclean disconnect.

It is NOT a broker. No QoS 2, no persistence, no wildcards beyond exact-topic matching, no
flow control, one client at a time is all it is really designed for. Do not leave it
running and do not point anything real at it.

    ./broker.py <certfile> <keyfile> <port> <username> <password>
"""
import json, socket, ssl, struct, sys, threading, time

CONNECT, CONNACK, PUBLISH, PUBACK, SUBSCRIBE, SUBACK, PINGREQ, PINGRESP, DISCONNECT = \
    1, 2, 3, 4, 8, 9, 12, 13, 14

log_lock = threading.Lock()
def log(*a):
    with log_lock:
        print(time.strftime('%H:%M:%S'), *a, flush=True)

# Everything the device has published, plus anything we retained on its behalf.
seen = []          # (topic, payload)
retained = {}

def read_exact(sock, n):
    buf = b''
    while len(buf) < n:
        d = sock.recv(n - len(buf))
        if not d:
            raise ConnectionError("peer closed")
        buf += d
    return buf

def read_remaining_length(sock):
    mult, value = 1, 0
    while True:
        b = read_exact(sock, 1)[0]
        value += (b & 0x7f) * mult
        if not (b & 0x80):
            return value
        mult *= 128
        if mult > 128 ** 3:
            raise ValueError("malformed remaining length")

def encode_remaining_length(n):
    out = b''
    while True:
        b = n % 128
        n //= 128
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out

def get_str(buf, i):
    (n,) = struct.unpack_from('!H', buf, i)
    return buf[i + 2:i + 2 + n].decode('utf-8', 'replace'), i + 2 + n

def send_packet(sock, kind, flags, payload):
    sock.sendall(bytes([(kind << 4) | flags]) + encode_remaining_length(len(payload)) + payload)

def publish_to(sock, topic, payload, retain=0):
    body = struct.pack('!H', len(topic)) + topic.encode() + payload
    send_packet(sock, PUBLISH, retain, body)

class Session:
    def __init__(self, sock, user, password):
        self.sock, self.user, self.password = sock, user, password
        self.will = None
        self.subs = []
        self.connected = False

    def handle_connect(self, body):
        i = 0
        proto, i = get_str(body, i)
        level = body[i]; i += 1
        flags = body[i]; i += 1
        (keepalive,) = struct.unpack_from('!H', body, i); i += 2
        client_id, i = get_str(body, i)
        self.keepalive = keepalive
        will_topic = will_msg = None
        if flags & 0x04:
            will_topic, i = get_str(body, i)
            (n,) = struct.unpack_from('!H', body, i)
            will_msg = body[i + 2:i + 2 + n]; i += 2 + n
        username = pw = None
        if flags & 0x80:
            username, i = get_str(body, i)
        if flags & 0x40:
            (n,) = struct.unpack_from('!H', body, i)
            pw = body[i + 2:i + 2 + n].decode(); i += 2 + n

        log(f"CONNECT proto={proto}v{level} id={client_id!r} keepalive={keepalive}s "
            f"user={username!r} will={will_topic!r} retain_will={bool(flags & 0x20)}")

        if username != self.user or pw != self.password:
            log(f"  -> REFUSED: bad credentials (got {username!r})")
            send_packet(self.sock, CONNACK, 0, bytes([0, 4]))   # 4 = bad user/password
            return False
        if will_topic:
            self.will = (will_topic, will_msg, bool(flags & 0x20))
        send_packet(self.sock, CONNACK, 0, bytes([0, 0]))
        # A real broker declares a silent client gone after 1.5x keepalive and publishes
        # its will. Without this the socket just blocks forever and a device that lost
        # power stays "online" -- which is the exact failure the will exists to prevent,
        # so a bench broker that cannot reproduce it is not much of a test.
        if keepalive:
            self.sock.settimeout(keepalive * 1.5)
        self.connected = True
        log("  -> accepted")
        return True

    def run(self):
        while True:
            b0 = read_exact(self.sock, 1)[0]
            kind, flags = b0 >> 4, b0 & 0x0f
            length = read_remaining_length(self.sock)
            body = read_exact(self.sock, length) if length else b''

            if kind == CONNECT:
                if not self.handle_connect(body):
                    return
            elif kind == SUBSCRIBE:
                (pid,) = struct.unpack_from('!H', body, 0)
                i, granted = 2, []
                while i < len(body):
                    topic, i = get_str(body, i)
                    qos = body[i]; i += 1
                    self.subs.append(topic)
                    granted.append(min(qos, 1))
                    log(f"SUBSCRIBE {topic!r} qos={qos}")
                send_packet(self.sock, SUBACK, 0, struct.pack('!H', pid) + bytes(granted))
            elif kind == PUBLISH:
                qos, retain = (flags >> 1) & 3, flags & 1
                topic, i = get_str(body, 0)
                if qos:
                    (pid,) = struct.unpack_from('!H', body, i); i += 2
                payload = body[i:]
                seen.append((topic, payload))
                if retain:
                    retained[topic] = payload
                log(f"PUBLISH <- {topic} retain={retain} qos={qos}: "
                    f"{payload.decode('utf-8','replace')[:200]}")
                if qos == 1:
                    send_packet(self.sock, PUBACK, 0, struct.pack('!H', pid))
            elif kind == PINGREQ:
                send_packet(self.sock, PINGRESP, 0, b'')
                log("PINGREQ -> PINGRESP")
            elif kind == DISCONNECT:
                log("DISCONNECT (clean -- no will)")
                self.will = None
                return
            else:
                log(f"ignoring packet type {kind}")

def serve(certfile, keyfile, port, user, password, cmd_file):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', port))
    srv.listen(4)
    log(f"[broker] TLS on :{port}, user {user!r}; drop a line in {cmd_file} to send a command")

    def handle(raw, addr):
        try:
            sock = ctx.wrap_socket(raw, server_side=True)
        except Exception as e:
            log(f"TLS handshake failed from {addr[0]}: {e}")
            raw.close()
            return
        log(f"--- connection from {addr[0]}")
        s = Session(sock, user, password)

        # Anything written to cmd_file (one "topic payload" per line) is published to the
        # device -- the bench equivalent of `mosquitto_pub`.
        stop = threading.Event()
        def pump():
            try:
                open(cmd_file, 'w').close()
            except OSError:
                pass
            while not stop.wait(0.3):
                try:
                    lines = [l for l in open(cmd_file).read().splitlines() if l.strip()]
                    if not lines:
                        continue
                    open(cmd_file, 'w').close()
                    for line in lines:
                        topic, _, payload = line.partition(' ')
                        log(f"PUBLISH -> {topic}: {payload!r}")
                        publish_to(sock, topic, payload.encode())
                except (OSError, ValueError, ssl.SSLError):
                    pass
        t = threading.Thread(target=pump, daemon=True); t.start()

        try:
            s.run()
        except Exception as e:
            log(f"connection ended: {e}")
            if s.will:
                topic, msg, retain = s.will
                retained[topic] = msg
                log(f"LAST WILL fired: {topic} = {msg.decode('utf-8','replace')!r} retain={retain}")
        finally:
            stop.set()
            try: sock.close()
            except Exception: pass
            with open(cmd_file + '.seen', 'w') as f:
                json.dump([[t, p.decode('utf-8', 'replace')] for t, p in seen], f, indent=1)
            with open(cmd_file + '.retained', 'w') as f:
                json.dump({t: p.decode('utf-8', 'replace') for t, p in retained.items()}, f, indent=1)

    # One thread per connection: a device that reboots reconnects immediately, and a
    # single-threaded accept loop would still be blocked reading from the socket it
    # already lost.
    while True:
        raw, addr = srv.accept()
        threading.Thread(target=handle, args=(raw, addr), daemon=True).start()

if __name__ == '__main__':
    serve(sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5],
          sys.argv[6] if len(sys.argv) > 6 else '/tmp/mqtt_cmd')
