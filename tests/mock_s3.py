#!/usr/bin/env python3
"""A minimal S3-compatible object store, used to exercise vcache's S3 layer.

It implements just enough for the cache: PUT and GET of a single object, plus
404 on a missing key. Requests must carry a SigV4 Authorization header and the
x-amz-content-sha256 payload hash, and the payload hash is verified, so a
malformed request fails the test rather than silently passing.

Signature *validity* is covered by known-answer tests in the unit suite; this
server checks request shape and round-tripping.

Usage: mock_s3.py <port> <storage-dir>
"""

import hashlib
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

STORAGE = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # keep test output clean

    def _object_path(self):
        # Path-style access: /<bucket>/<key...>
        parts = self.path.lstrip("/").split("/", 1)
        if len(parts) != 2 or not parts[1]:
            return None
        safe = parts[1].replace("..", "_")
        return os.path.join(STORAGE, safe.replace("/", "__"))

    def _check_headers(self, body):
        if "Authorization" not in self.headers:
            self.send_error(403, "missing Authorization")
            return False
        auth = self.headers["Authorization"]
        if not auth.startswith("AWS4-HMAC-SHA256 "):
            self.send_error(403, "unexpected signature version")
            return False
        for required in ("Credential=", "SignedHeaders=", "Signature="):
            if required not in auth:
                self.send_error(403, f"Authorization missing {required}")
                return False
        if "x-amz-date" not in self.headers:
            self.send_error(403, "missing x-amz-date")
            return False
        claimed = self.headers.get("x-amz-content-sha256")
        if claimed is None:
            self.send_error(403, "missing x-amz-content-sha256")
            return False
        actual = hashlib.sha256(body).hexdigest()
        if claimed != actual:
            self.send_error(400, "payload hash mismatch")
            return False
        return True

    def do_GET(self):
        if not self._check_headers(b""):
            return
        path = self._object_path()
        if path is None or not os.path.exists(path):
            self.send_error(404, "not found")
            return
        with open(path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        if not self._check_headers(body):
            return
        path = self._object_path()
        if path is None:
            self.send_error(400, "bad key")
            return
        with open(path, "wb") as f:
            f.write(body)
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


def main():
    global STORAGE
    port = int(sys.argv[1])
    STORAGE = sys.argv[2]
    os.makedirs(STORAGE, exist_ok=True)
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
