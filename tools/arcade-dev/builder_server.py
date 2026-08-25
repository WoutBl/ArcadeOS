#!/usr/bin/env python3
"""
builder_server — local bridge for the no-code game builder website.

Serves tools/arcade-dev/game-builder.html and handles POST /build:
  1. codegen.py turns the posted game definition into C (struct
     literals only — see sdk/arcade_builder.h/.c for the engine).
  2. Compiles it with the exact flags `arcade build` already uses
     against build/libarcade.a (stdlib subprocess, no new toolchain
     dependency).
  3. Runs tools/pack_title.py on the result (same display-title
     trailer every other game gets).
  4. POSTs the built ELF to http://<console-ip>:<port>/api/upload —
     the same endpoint `curl` already talks to.

Because this process serves the page AND the API on one origin, the
browser's requests are same-origin — no CORS wrinkle, no changes
needed to src/net.c.

Usage: builder_server.py [--port 8765]
"""
import http.server
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
import webbrowser

TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))  # repo root
GEN_DIR = os.path.join(ROOT, "build", "generated")

sys.path.insert(0, TOOL_DIR)
import codegen  # noqa: E402  (needs TOOL_DIR on sys.path first)

NOFPU = ["-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2", "-msoft-float"]
APP_LDFLAGS = "-Wl,-Ttext=0x401000,--build-id=none,-e,main,-z,max-page-size=0x1000"

DEFAULT_CONSOLE_IP = "10.0.2.86"
DEFAULT_CONSOLE_PORT = 8080


def ensure_sdk():
    """Same as arcade's ensure_sdk(): (re)build build/libarcade.a."""
    lib = os.path.join(ROOT, "build", "libarcade.a")
    r = subprocess.run(["make", "-C", ROOT, "build/libarcade.a"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"could not build the SDK:\n{r.stdout}")
    return lib


def build_and_deploy(game, target):
    """game: parsed game-definition dict. target: {"ip":..,"port":..}.
    Returns a result dict; never raises — errors come back as {"ok":False,...}."""
    os.makedirs(GEN_DIR, exist_ok=True)

    try:
        name = codegen.game_tag(game.get("name", ""))
    except codegen.BuildError as e:
        return {"ok": False, "stage": "codegen", "error": str(e)}

    c_path = os.path.join(GEN_DIR, f"{name}.c")
    elf_path = os.path.join(GEN_DIR, f"{name}.elf")

    try:
        code = codegen.generate(game)
    except codegen.BuildError as e:
        return {"ok": False, "stage": "codegen", "error": str(e)}
    with open(c_path, "w") as f:
        f.write(code)

    try:
        lib = ensure_sdk()
    except RuntimeError as e:
        return {"ok": False, "stage": "sdk", "error": str(e)}

    r = subprocess.run(
        ["x86_64-elf-gcc", "-Os", "-s", "-ffreestanding", "-nostdlib",
         "-fno-builtin", *NOFPU,
         "-I", os.path.join(ROOT, "sdk"), "-I", os.path.join(ROOT, "libc"),
         c_path, lib, "-o", elf_path, APP_LDFLAGS],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        return {"ok": False, "stage": "compile", "error": r.stdout}

    r = subprocess.run(["python3", os.path.join(ROOT, "tools", "pack_title.py"),
                        elf_path, c_path],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        return {"ok": False, "stage": "pack_title", "error": r.stdout}

    with open(elf_path, "rb") as f:
        elf_bytes = f.read()

    ip = target.get("ip") or DEFAULT_CONSOLE_IP
    port = int(target.get("port") or DEFAULT_CONSOLE_PORT)
    filename = f"{name}.ELF"
    url = f"http://{ip}:{port}/api/upload?name={filename}"
    try:
        req = urllib.request.Request(url, data=elf_bytes, method="POST")
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError) as e:
        return {"ok": False, "stage": "upload", "error": f"{ip}:{port}: {e}"}

    return {"ok": True, "file": filename, "bytes": len(elf_bytes), "console": body}


class Handler(http.server.BaseHTTPRequestHandler):
    def _send_json(self, status, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = "/game-builder.html" if self.path in ("/", "") else self.path
        fs_path = os.path.join(TOOL_DIR, path.lstrip("/"))
        if not os.path.abspath(fs_path).startswith(TOOL_DIR) or not os.path.isfile(fs_path):
            self.send_error(404)
            return
        ctype = "text/html" if fs_path.endswith(".html") else "application/octet-stream"
        with open(fs_path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if self.path != "/build":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", 0))
        try:
            payload = json.loads(self.rfile.read(length))
        except json.JSONDecodeError as e:
            self._send_json(400, {"ok": False, "stage": "request", "error": str(e)})
            return

        game = payload.get("game")
        target = payload.get("target", {})
        if not isinstance(game, dict):
            self._send_json(400, {"ok": False, "stage": "request",
                                  "error": "missing \"game\" in request body"})
            return

        result = build_and_deploy(game, target)
        print(f"builder_server: {game.get('name')!r} -> {result.get('ok')} "
             f"({result.get('stage', 'deploy')})")
        self._send_json(200 if result["ok"] else 422, result)

    def log_message(self, fmt, *args):
        pass  # keep stdout to our own one-line-per-build summaries


def main():
    port = 8765
    if "--port" in sys.argv:
        port = int(sys.argv[sys.argv.index("--port") + 1])

    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    url = f"http://127.0.0.1:{port}/"
    print(f"builder_server: serving {url} (Ctrl-C to stop)")
    if "--no-browser" not in sys.argv:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
