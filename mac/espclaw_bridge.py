#!/usr/bin/env python3
"""Restricted, authenticated LAN bridge to this Mac's local Qwen model."""

import argparse
import base64
import binascii
import hmac
import ipaddress
import json
import os
from pathlib import Path
import secrets
import socket
import stat
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import urllib.error
import urllib.request


MODEL = "qwen3.8-27b"
UPSTREAM_URL = "http://127.0.0.1:1234/v1/chat/completions"
DEFAULT_CONFIG = Path(__file__).resolve().parent / "private" / "bridge.json"
MAX_BODY_BYTES = 1024 * 1024
MAX_REPLY_BYTES = 4 * 1024 * 1024
MAX_TOKENS = 512
UPSTREAM_TIMEOUT = 180
LAN_RANGES = tuple(ipaddress.ip_network(s) for s in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


class RequestProblem(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


class UpstreamProblem(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def validate_bind(value):
    try:
        address = ipaddress.ip_address(value)
    except ValueError:
        raise ValueError("Bind must be an explicit private IPv4 LAN address") from None
    if address.version != 4 or not any(address in network for network in LAN_RANGES):
        raise ValueError("Bind must be a private LAN address in 10/8, 172.16/12, or 192.168/16")
    return str(address)


def read_config(path):
    path = Path(path)
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) != 0o600:
        raise ValueError("Bridge config must be a regular file with permissions 0600")
    if info.st_size > 8192:
        raise ValueError("Bridge config is too large")
    config = json.loads(path.read_text(encoding="utf-8"))
    bind = validate_bind(config.get("bind", ""))
    port = config.get("port", 1235)
    if type(port) is not int or not 1024 <= port <= 65535 or port == 1234:
        raise ValueError("Bridge port must be 1024–65535, excluding LM Studio's port 1234")
    token = config.get("token")
    if not isinstance(token, str) or len(token) < 32 or not token.isascii() or any(c.isspace() for c in token):
        raise ValueError("Bridge token must be an ASCII secret of at least 32 characters")
    return {"bind": bind, "port": port, "token": token}


def initialize_config(path, bind, port):
    bind = validate_bind(bind)
    if not 1024 <= port <= 65535 or port == 1234:
        raise ValueError("Choose a port from 1024–65535 other than 1234")
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    path.parent.chmod(0o700)
    config = {"bind": bind, "port": port, "token": secrets.token_urlsafe(32)}
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
        json.dump(config, handle, indent=2)
        handle.write("\n")


def validate_inline_images(messages):
    for message in messages:
        if not isinstance(message, dict) or not isinstance(message.get("role"), str):
            raise RequestProblem(400, "Each message must have a role")
        content = message.get("content")
        if content is None or isinstance(content, str):
            continue
        if not isinstance(content, list):
            raise RequestProblem(400, "Message content must be text or a content array")
        for part in content:
            if not isinstance(part, dict):
                raise RequestProblem(400, "Invalid message content part")
            if part.get("type") == "text" and isinstance(part.get("text"), str):
                continue
            if part.get("type") != "image_url" or not isinstance(part.get("image_url"), dict):
                raise RequestProblem(400, "Only text and inline image_url content are supported")
            url = part["image_url"].get("url")
            if not isinstance(url, str):
                raise RequestProblem(400, "Image URL must be an inline base64 data URL")
            prefix, separator, encoded = url.partition(",")
            allowed = {"data:image/jpeg;base64", "data:image/png;base64", "data:image/webp;base64", "data:image/gif;base64"}
            if not separator or prefix not in allowed or not encoded:
                raise RequestProblem(400, "Images must be inline base64 JPEG, PNG, WebP, or GIF; remote URLs are not accepted")
            try:
                base64.b64decode(encoded, validate=True)
            except (binascii.Error, ValueError):
                raise RequestProblem(400, "Invalid image base64 data") from None


def prepare_payload(body):
    if not isinstance(body, dict):
        raise RequestProblem(400, "Request JSON must be an object")
    messages = body.get("messages")
    if not isinstance(messages, list) or not messages or len(messages) > 128:
        raise RequestProblem(400, "Provide between 1 and 128 messages")
    validate_inline_images(messages)
    requested_tokens = body.get("max_tokens", body.get("max_completion_tokens", 256))
    if type(requested_tokens) is not int or requested_tokens < 1:
        raise RequestProblem(400, "max_tokens must be a positive integer")
    if "tools" in body and (not isinstance(body["tools"], list) or len(body["tools"]) > 64):
        raise RequestProblem(400, "tools must be a list containing at most 64 tools")
    allowed = (
        "messages", "tools", "tool_choice", "temperature", "top_p", "top_k", "min_p",
        "repeat_penalty", "presence_penalty", "frequency_penalty", "seed", "stop", "response_format",
    )
    payload = {key: body[key] for key in allowed if key in body}
    payload.update({
        "model": MODEL,
        "stream": False,
        "reasoning_effort": "none",
        "parallel_tool_calls": False,
        "max_tokens": min(requested_tokens, MAX_TOKENS),
    })
    return payload


def forward_to_lm(payload):
    req = urllib.request.Request(
        UPSTREAM_URL,
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
        method="POST",
    )
    # Never send the board's token upstream. Ignore environment proxies and redirects.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    try:
        with opener.open(req, timeout=UPSTREAM_TIMEOUT) as response:
            raw = response.read(MAX_REPLY_BYTES + 1)
        if len(raw) > MAX_REPLY_BYTES:
            raise UpstreamProblem(502, "Local model response exceeded the bridge limit")
        result = json.loads(raw)
        if not isinstance(result, dict):
            raise ValueError("Invalid response")
        if "error" in result:
            raise UpstreamProblem(502, "Local model rejected the request")
        return result
    except urllib.error.HTTPError as exc:
        # Upstream error bodies may echo a prompt. Do not relay or log them.
        status = 400 if exc.code in (400, 413, 422) else 502
        raise UpstreamProblem(status, "Local model rejected the request") from None
    except (TimeoutError, socket.timeout):
        raise UpstreamProblem(504, "Local model request timed out") from None
    except urllib.error.URLError as exc:
        status = 504 if isinstance(exc.reason, (TimeoutError, socket.timeout)) else 502
        raise UpstreamProblem(status, "Local model is unavailable or timed out") from None
    except (ValueError, OSError):
        raise UpstreamProblem(502, "Local model returned an invalid response") from None


class BridgeServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, token, forwarder=forward_to_lm):
        self.token = token
        self.forwarder = forwarder
        self.inference_slot = threading.BoundedSemaphore(1)
        super().__init__(address, BridgeHandler)

    def handle_error(self, request, client_address):
        # Avoid traceback/request dumps that might include prompts or credentials.
        print("Bridge connection failed.", flush=True)


class BridgeHandler(BaseHTTPRequestHandler):
    server_version = "ESPClawBridge"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def log_message(self, format, *args):
        pass

    def send_json(self, status, body):
        encoded = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        if status == 401:
            self.send_header("WWW-Authenticate", "Bearer")
        if status == 429:
            self.send_header("Retry-After", "5")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(encoded)

    def error(self, status, message):
        self.send_json(status, {"error": {"message": message, "type": "bridge_error"}})

    def do_GET(self):
        if self.path != "/health":
            self.error(404, "Not found")
            return
        self.send_json(200, {"status": "ok"})

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.error(404, "Not found")
            return
        authorizations = self.headers.get_all("Authorization", [])
        expected = "Bearer " + self.server.token
        if len(authorizations) != 1 or not hmac.compare_digest(authorizations[0].encode("utf-8"), expected.encode("ascii")):
            self.error(401, "Authentication required")
            return
        try:
            if self.headers.get("Transfer-Encoding") is not None:
                raise RequestProblem(400, "Transfer-Encoding is not supported; send Content-Length")
            lengths = self.headers.get_all("Content-Length", [])
            if len(lengths) != 1 or not lengths[0].isdigit():
                raise RequestProblem(411, "A valid Content-Length is required")
            length = int(lengths[0])
            if length > MAX_BODY_BYTES:
                raise RequestProblem(413, "Request exceeds the 1 MiB limit")
            if not length:
                raise RequestProblem(400, "Request body is empty")
            if self.headers.get_content_type() != "application/json":
                raise RequestProblem(415, "Content-Type must be application/json")
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise RequestProblem(400, "Incomplete request body")
            try:
                body = json.loads(raw)
            except (ValueError, UnicodeError):
                raise RequestProblem(400, "Invalid JSON") from None
            payload = prepare_payload(body)
        except RequestProblem as exc:
            self.error(exc.status, str(exc))
            return
        except (TimeoutError, socket.timeout):
            self.error(408, "Request body timed out")
            return
        if not self.server.inference_slot.acquire(blocking=False):
            self.error(429, "Local model is busy; retry shortly")
            return
        try:
            result = self.server.forwarder(payload)
            self.send_json(200, result)
        except UpstreamProblem as exc:
            self.error(exc.status, str(exc))
        except Exception:
            self.error(502, "Local model request failed")
        finally:
            self.server.inference_slot.release()

    def unsupported_method(self):
        self.error(405, "Method not allowed")

    do_PUT = do_DELETE = do_PATCH = do_OPTIONS = do_HEAD = unsupported_method


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    actions = parser.add_subparsers(dest="action", required=True)
    init = actions.add_parser("init", help="Create a private config and random token; does not start the bridge")
    init.add_argument("--bind", required=True, help="This Mac's explicit private IPv4 LAN address")
    init.add_argument("--port", type=int, default=1235)
    actions.add_parser("serve", help="Start the bridge using the private config")
    args = parser.parse_args()
    try:
        if args.action == "init":
            initialize_config(args.config, args.bind, args.port)
            print(f"Created private config: {args.config}. The token is not printed.")
            return
        config = read_config(args.config)
        server = BridgeServer((config["bind"], config["port"]), config["token"])
    except (ValueError, OSError, TypeError) as exc:
        parser.exit(1, f"Bridge setup failed: {exc}\n")
    print(f"ESPClaw bridge listening on {config['bind']}:{config['port']} for {MODEL}; Ctrl-C stops it.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping bridge.", flush=True)
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
