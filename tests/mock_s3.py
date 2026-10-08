#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Unto Labs
# SPDX-License-Identifier: Apache-2.0
"""A minimal S3-compatible object store, used to exercise vcache's S3 layer.

It implements just enough for the cache: PUT, GET and DELETE of a single
object, ListObjectsV2 with continuation, plus 404 on a missing key. Requests
must carry a SigV4 Authorization header and the x-amz-content-sha256 payload
hash, and the payload hash is verified, so a malformed request fails the test
rather than silently passing.

Signature *validity* is covered by known-answer tests in the unit suite; this
server checks request shape and round-tripping.

Usage: mock_s3.py <port> <storage-dir>

For benchmarks, two settings make it behave more like a distant bucket:
MOCK_S3_LATENCY_MS delays every response, standing in for the round trip, and
MOCK_S3_HANDSHAKE_MS delays the first request on each connection, standing in
for TCP and TLS setup. Either one also turns on HTTP/1.1 keep-alive and a
thread per connection, as a real endpoint has, so a client that reuses its
connection pays the handshake once. Both default to off, which keeps the
original single-threaded HTTP/1.0 server the tests were written against.
"""

import hashlib
import os
import signal
import socket
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

STORAGE = None
TRANSIENT_PUTS = {}


LATENCY_S = int(os.environ.get("MOCK_S3_LATENCY_MS", "0")) / 1000.0
HANDSHAKE_S = int(os.environ.get("MOCK_S3_HANDSHAKE_MS", "0")) / 1000.0
REALISTIC = LATENCY_S > 0 or HANDSHAKE_S > 0


class Handler(BaseHTTPRequestHandler):
    if REALISTIC:
        protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass  # keep test output clean

    def setup(self):
        super().setup()
        if REALISTIC:
            # Headers and body go out as two writes. With Nagle on, the body
            # waits for the client to ACK the headers, which on a reused
            # connection means its delayed-ACK timer -- a stall a real endpoint
            # does not have, and one that would penalise exactly the clients
            # that keep their connections.
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if HANDSHAKE_S:
            time.sleep(HANDSHAKE_S)

    def end_headers(self):
        # A put that applies after its latency has already slept, and must not
        # sleep again here or the completion order would not match the write.
        if LATENCY_S and not getattr(self, "_latency_applied", False):
            time.sleep(LATENCY_S)
        super().end_headers()

    def _object_path(self):
        # Path-style access: /<bucket>/<key...>
        parts = urlparse(self.path).path.lstrip("/").split("/", 1)
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

    def _listing(self):
        """ListObjectsV2 for the whole bucket, honouring ?prefix=.

        Continuation is implemented with a one-object page size so the test can
        exercise the paging path without needing a thousand objects.
        """
        query = parse_qs(urlparse(self.path).query)
        prefix = query.get("prefix", [""])[0]
        after = query.get("continuation-token", [""])[0]

        keys = sorted(os.listdir(STORAGE))
        entries = []
        for stored in keys:
            key = stored.replace("__", "/")
            if not key.startswith(prefix):
                continue
            entries.append((key, stored))

        page_size = int(os.environ.get("MOCK_S3_PAGE_SIZE", "1000"))
        start = 0
        if after:
            for i, (key, _) in enumerate(entries):
                if key == after:
                    start = i + 1
                    break
        page = entries[start:start + page_size]
        truncated = start + page_size < len(entries)

        out = ['<?xml version="1.0" encoding="UTF-8"?>',
               '<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">']
        out.append(f"<IsTruncated>{'true' if truncated else 'false'}</IsTruncated>")
        for key, stored in page:
            full = os.path.join(STORAGE, stored)
            st = os.stat(full)
            when = time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime(st.st_mtime))
            out.append("<Contents>")
            out.append(f"<Key>{key}</Key>")
            out.append(f"<LastModified>{when}</LastModified>")
            out.append(f"<Size>{st.st_size}</Size>")
            out.append("</Contents>")
        if truncated and page:
            out.append(f"<NextContinuationToken>{page[-1][0]}</NextContinuationToken>")
        out.append("</ListBucketResult>")
        return "".join(out).encode()

    def do_GET(self):
        if not self._check_headers(b""):
            return
        no_list = os.environ.get("MOCK_S3_NO_LISTBUCKET") == "1"
        if "list-type=2" in urlparse(self.path).query:
            # A bucket without s3:ListBucket denies the listing itself.
            if no_list:
                self.send_error(403, "AccessDenied")
                return
            body = self._listing()
            self.send_response(200)
            self.send_header("Content-Type", "application/xml")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        path = self._object_path()
        if path is None or not os.path.exists(path):
            # Real S3 answers a missing key with 403 rather than 404 when the
            # caller lacks ListBucket. That ambiguity is what the diagnostic in
            # S3Storage::DiagnoseDenial exists to resolve.
            self.send_error(403 if no_list else 404, "not found")
            return
        with open(path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        # Real S3 always sends this; vcache's TTL check depends on it, and the
        # test backdates object mtimes to drive that check.
        self.send_header(
            "Last-Modified",
            time.strftime("%a, %d %b %Y %H:%M:%S GMT",
                          time.gmtime(os.stat(path).st_mtime)))
        self.end_headers()
        self.wfile.write(data)

    def do_DELETE(self):
        if not self._check_headers(b""):
            return
        path = self._object_path()
        if path is None:
            self.send_error(400, "bad key")
            return
        # S3 returns 204 whether or not the key existed.
        if os.path.exists(path):
            os.remove(path)
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_PUT(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        if not self._check_headers(body):
            return
        # 403 is a miss with no error string. A refused synchronous put must
        # still count as a failed upload.
        deny_min = int(os.environ.get("MOCK_S3_DENY_PUT_MIN_BYTES", "0") or "0")
        if deny_min and len(body) >= deny_min:
            self.send_error(403, "AccessDenied")
            return
        path = self._object_path()
        if path is None:
            self.send_error(400, "bad key")
            return
        # A refused re-put has to see that this attempt started, including
        # when the attempt answers 503 and never writes the object.
        apply_after = os.environ.get("MOCK_S3_APPLY_AFTER_LATENCY") == "1"
        if apply_after:
            with open(path + ".started", "w", encoding="ascii"):
                pass
        # MOCK_S3_TRANSIENT_PUT_FAILURES=N makes the first N PUTs answer 503
        # SlowDown, which is what a real bucket does when it is shedding load
        # rather than rejecting the request. Counted per object so a test can
        # assert the retry lands the entry rather than merely surviving.
        budget = int(os.environ.get("MOCK_S3_TRANSIENT_PUT_FAILURES", "0"))
        if budget:
            key = self.path
            seen = TRANSIENT_PUTS.get(key, 0)
            if seen < budget:
                TRANSIENT_PUTS[key] = seen + 1
                payload = (b'<?xml version="1.0" encoding="UTF-8"?>'
                           b"<Error><Code>SlowDown</Code>"
                           b"<Message>Please reduce your request rate.</Message></Error>")
                self.send_response(503)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
        # The object appears when the request finishes, not when it arrives, so
        # a slow upload that started first can still be the value left behind.
        if apply_after:
            delay = LATENCY_S
            slow_under = int(os.environ.get("MOCK_S3_SLOW_UNDER_BYTES", "0") or "0")
            slow_extra = int(os.environ.get("MOCK_S3_SLOW_UNDER_EXTRA_MS", "0") or "0")
            if slow_under and len(body) < slow_under:
                delay += slow_extra / 1000.0
            if delay:
                time.sleep(delay)
            self._latency_applied = True
        with open(path, "wb") as f:
            f.write(body)
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


def main():
    # A parent that ignores SIGTERM hands that disposition to this process.
    # kill then does nothing, and the suite's wait sits on the pid.
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    signal.signal(signal.SIGHUP, signal.SIG_DFL)
    global STORAGE
    port = int(sys.argv[1])
    STORAGE = sys.argv[2]
    os.makedirs(STORAGE, exist_ok=True)
    server = ThreadingHTTPServer if REALISTIC else HTTPServer
    server(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
