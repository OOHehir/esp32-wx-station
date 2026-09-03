#!/usr/bin/env python3
"""Stand-in for the NAS while testing the firmware side of OTA.

Serves the same /fw/<project>/<chip>/... layout tools/publish.sh writes, over TLS, with
a certificate signed by the same private CA the firmware embeds. It is NOT a replacement
for nas/nginx.conf -- it exists so the device half can be proven without write access to
the NAS.

Threaded on purpose: the device holds the image connection open for the whole download,
and a single-threaded server would then refuse the browser polling /api/status.
"""
import http.server, os, socketserver, ssl, sys, threading

ROOT = sys.argv[1]
CERT, KEY = sys.argv[2], sys.argv[3]
PORT = int(sys.argv[4]) if len(sys.argv) > 4 else 8443


class Handler(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        path = path.split("?", 1)[0].split("#", 1)[0]
        if path == "/healthz":
            return "/healthz"
        if path.startswith("/fw/"):
            path = path[4:]
        rel = os.path.normpath(path).lstrip("/")
        # Containment is checked here because overriding translate_path discards
        # SimpleHTTPRequestHandler's own traversal filtering, and normpath keeps a leading
        # "..". Without this, GET /fw/../../../keys/app_signing_rsa3072.pem serves the OTA
        # signing key -- and this listens on 0.0.0.0 with no authentication.
        root = os.path.realpath(ROOT)
        full = os.path.realpath(os.path.join(root, rel))
        if full != root and not full.startswith(root + os.sep):
            return os.path.join(root, "__outside_root__")
        return full

    def do_GET(self):
        if self.path.split("?")[0] == "/healthz":
            body = b"ok\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def end_headers(self):
        if self.path.endswith("manifest.json"):
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("  [rig] %s %s\n" % (self.address_string(), fmt % args))
        sys.stderr.flush()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ctx.load_cert_chain(CERT, KEY)

httpd = Server(("0.0.0.0", PORT), Handler)
httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
sys.stderr.write("  [rig] serving %s on https://0.0.0.0:%d/fw/\n" % (ROOT, PORT))
sys.stderr.flush()
httpd.serve_forever()
