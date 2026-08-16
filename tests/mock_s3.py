#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Unto Labs
# SPDX-License-Identifier: GPL-3.0-only
"""A minimal S3-compatible object store, used to exercise vcache's S3 layer.

It implements just enough for the cache: PUT, GET and DELETE of a single
object, ListObjectsV2 with continuation, plus 404 on a missing key. Requests
must carry a SigV4 Authorization header and the x-amz-content-sha256 payload
hash, and the payload hash is verified, so a malformed request fails the test
rather than silently passing.

Signature *validity* is covered by known-answer tests in the unit suite; this
server checks request shape and round-tripping.

Usage: mock_s3.py <port> <storage-dir>
"""

import hashlib
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

STORAGE = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # keep test output clean

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
