#!/usr/bin/env python3
"""Static file server for wasm builds.

Sends the cross-origin-isolation headers a threaded (SharedArrayBuffer) build
requires, the correct wasm MIME type, and no-store cache headers so a rebuild
is always what the browser runs.

    python3 Tools/Web/serve.py --root build/wasm-debug/bin --port 8080
"""

import argparse
import http.server
import os


class WasmHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        # require-corp for production parity (Safari implements only this
        # spelling; the Web panel's iframe carries its own credentialless
        # attribute, which is what actually governs nested documents).
        # Previous rationale, kept for context:
        # credentialless rather than require-corp. Both confer the cross-origin
        # isolation SharedArrayBuffer needs, but require-corp additionally
        # demands that every embedded subresource opt in with its own
        # Cross-Origin-Resource-Policy header — which ordinary websites do not
        # send, so the editor's Web panel iframe is blocked before it loads and
        # the panel sits there empty. credentialless drops that demand and
        # sends cross-origin subrequests without credentials instead.
        # Chromium-only: Firefox confers isolation for require-corp alone, so a
        # Firefox host would have to choose between threads and the panel.
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=".", help="directory to serve")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="127.0.0.1")
    args = parser.parse_args()

    os.chdir(args.root)
    server = http.server.ThreadingHTTPServer((args.bind, args.port), WasmHandler)
    print(f"serving {os.getcwd()} at http://{args.bind}:{args.port}/ (COOP/COEP on)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
