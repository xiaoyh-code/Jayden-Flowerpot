"""Tests use a loopback-only server and mock LM Studio; no inference or LAN listener."""

import base64
from contextlib import contextmanager
import http.client
import io
import json
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch
import urllib.error

import espclaw_bridge as bridge


TOKEN = "synthetic-test-token-never-used-in-production"
REPLY = {"id": "test", "choices": [{"message": {"role": "assistant", "content": "ACK"}, "finish_reason": "stop"}]}


@contextmanager
def running_bridge(forwarder=None):
    forwarder = forwarder or Mock(return_value=REPLY)
    server = bridge.BridgeServer(("127.0.0.1", 0), TOKEN, forwarder=forwarder)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()
    try:
        yield server, forwarder
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def request(server, body=None, *, method="POST", path="/v1/chat/completions", auth=TOKEN, extra_headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
    headers = {"Content-Type": "application/json"}
    if auth is not None:
        headers["Authorization"] = "Bearer " + auth
    if extra_headers:
        headers.update(extra_headers)
    raw = None if body is None else json.dumps(body).encode("utf-8")
    try:
        connection.request(method, path, body=raw, headers=headers)
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), json.loads(response.read())
    finally:
        connection.close()


class BridgeHTTPTests(unittest.TestCase):
    def test_health_is_minimal_and_routes_are_restricted(self):
        with running_bridge() as (server, upstream):
            status, headers, body = request(server, method="GET", path="/health", auth=None)
            self.assertEqual((status, body), (200, {"status": "ok"}))
            self.assertNotIn("Access-Control-Allow-Origin", headers)
            self.assertEqual(request(server, method="GET", path="/v1/models")[0], 404)
            self.assertEqual(request(server, {}, path="/api/v1/models/load")[0], 404)
            self.assertEqual(request(server, {}, method="DELETE")[0], 405)
            upstream.assert_not_called()

    def test_missing_wrong_and_duplicate_auth_are_rejected(self):
        with running_bridge() as (server, upstream):
            for token in (None, "wrong-token"):
                self.assertEqual(request(server, {"messages": []}, auth=token)[0], 401)
            connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
            try:
                connection.putrequest("POST", "/v1/chat/completions")
                connection.putheader("Authorization", "Bearer " + TOKEN)
                connection.putheader("Authorization", "Bearer " + TOKEN)
                connection.putheader("Content-Length", "0")
                connection.endheaders()
                self.assertEqual(connection.getresponse().status, 401)
            finally:
                connection.close()
            upstream.assert_not_called()

    def test_normal_request_enforces_model_and_generation_limits(self):
        body = {
            "model": "cloud-or-other-model", "messages": [{"role": "user", "content": "Hello"}],
            "stream": True, "reasoning_effort": "xhigh", "parallel_tool_calls": True,
            "max_tokens": 999999, "upstream_url": "https://example.invalid", "temperature": 0.2,
        }
        with running_bridge() as (server, upstream):
            status, _, result = request(server, body)
            self.assertEqual((status, result), (200, REPLY))
            sent = upstream.call_args.args[0]
            self.assertEqual(sent["model"], "qwen3.8-27b")
            self.assertEqual(sent["max_tokens"], 512)
            self.assertEqual(sent["reasoning_effort"], "none")
            self.assertIs(sent["stream"], False)
            self.assertIs(sent["parallel_tool_calls"], False)
            self.assertEqual(sent["messages"], body["messages"])
            self.assertEqual(sent["temperature"], 0.2)
            self.assertNotIn("upstream_url", sent)

    def test_inline_image_and_agent_tools_are_preserved(self):
        image_url = "data:image/png;base64," + base64.b64encode(b"synthetic-image-bytes").decode()
        body = {
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": "Describe this."},
                {"type": "image_url", "image_url": {"url": image_url, "detail": "low"}},
            ]}],
            "tools": [{"type": "function", "function": {"name": "take_picture", "parameters": {"type": "object"}}}],
            "tool_choice": "auto", "max_tokens": 128,
        }
        with running_bridge() as (server, upstream):
            self.assertEqual(request(server, body)[0], 200)
            sent = upstream.call_args.args[0]
            for key in ("messages", "tools", "tool_choice", "max_tokens"):
                self.assertEqual(sent[key], body[key])

    def test_remote_image_and_invalid_base64_are_rejected(self):
        with running_bridge() as (server, upstream):
            for url in ("https://example.invalid/camera.jpg", "http://192.168.1.3/camera.jpg", "data:image/png;base64,!!!!"):
                body = {"messages": [{"role": "user", "content": [{"type": "image_url", "image_url": {"url": url}}]}]}
                self.assertEqual(request(server, body)[0], 400)
            upstream.assert_not_called()

    def test_size_is_rejected_before_reading_body_and_upstream(self):
        with running_bridge() as (server, upstream):
            status, _, _ = request(server, {}, extra_headers={"Content-Length": str(bridge.MAX_BODY_BYTES + 1)})
            self.assertEqual(status, 413)
            status, _, _ = request(server, {}, extra_headers={"Transfer-Encoding": "chunked"})
            self.assertEqual(status, 400)
            upstream.assert_not_called()

    def test_malformed_input_does_not_reach_upstream(self):
        with running_bridge() as (server, upstream):
            for body in ([], {}, {"messages": []}, {"messages": [{"role": "user", "content": "hi"}], "max_tokens": True}):
                self.assertEqual(request(server, body)[0], 400)
            upstream.assert_not_called()

    def test_only_one_upstream_request_at_a_time_and_slot_recovers(self):
        entered, release = threading.Event(), threading.Event()
        def blocked(payload):
            entered.set()
            if not release.wait(timeout=3):
                raise RuntimeError("Test timeout")
            return REPLY
        first = []
        with running_bridge(blocked) as (server, _):
            body = {"messages": [{"role": "user", "content": "hi"}]}
            thread = threading.Thread(target=lambda: first.append(request(server, body)), daemon=True)
            thread.start()
            try:
                self.assertTrue(entered.wait(timeout=2))
                status, headers, _ = request(server, body)
                self.assertEqual(status, 429)
                self.assertEqual(headers.get("Retry-After"), "5")
            finally:
                release.set()
                thread.join(timeout=3)
            self.assertEqual(first[0][0], 200)
            self.assertEqual(request(server, body)[0], 200)

    def test_upstream_error_does_not_leak_private_details(self):
        with running_bridge(Mock(side_effect=RuntimeError("secret-token private-prompt"))) as (server, _):
            status, _, body = request(server, {"messages": [{"role": "user", "content": "hi"}]})
            self.assertEqual(status, 502)
            self.assertNotIn("secret", json.dumps(body))
            self.assertNotIn("private", json.dumps(body))


class UpstreamTests(unittest.TestCase):
    @patch.object(bridge.urllib.request, "build_opener")
    def test_fixed_loopback_destination_timeout_and_no_bearer_forwarding(self, build_opener):
        response = io.BytesIO(json.dumps(REPLY).encode())
        build_opener.return_value.open.return_value = response
        payload = bridge.prepare_payload({"messages": [{"role": "user", "content": "hi"}]})
        self.assertEqual(bridge.forward_to_lm(payload), REPLY)
        req = build_opener.return_value.open.call_args.args[0]
        self.assertEqual(req.full_url, "http://127.0.0.1:1234/v1/chat/completions")
        self.assertIsNone(req.get_header("Authorization"))
        self.assertEqual(build_opener.return_value.open.call_args.kwargs["timeout"], 180)
        proxy_handler, redirect_handler = build_opener.call_args.args
        self.assertEqual(proxy_handler.proxies, {})
        self.assertIsInstance(redirect_handler, bridge.NoRedirect)
        self.assertIsNone(redirect_handler.redirect_request(None, None, 302, "", {}, "https://example.invalid"))

    @patch.object(bridge.urllib.request, "build_opener")
    def test_upstream_timeout_is_504(self, build_opener):
        build_opener.return_value.open.side_effect = socket.timeout()
        with self.assertRaises(bridge.UpstreamProblem) as result:
            bridge.forward_to_lm({})
        self.assertEqual(result.exception.status, 504)

    @patch.object(bridge.urllib.request, "build_opener")
    def test_upstream_http_error_body_is_discarded(self, build_opener):
        build_opener.return_value.open.side_effect = urllib.error.HTTPError(
            bridge.UPSTREAM_URL, 400, "Bad request", {}, io.BytesIO(b"sensitive prompt/token")
        )
        with self.assertRaises(bridge.UpstreamProblem) as result:
            bridge.forward_to_lm({})
        self.assertEqual(result.exception.status, 400)
        self.assertNotIn("sensitive", str(result.exception))


class ConfigTests(unittest.TestCase):
    def test_only_explicit_private_lan_addresses_are_accepted(self):
        for address in ("192.168.1.20", "10.3.2.1", "172.16.0.8"):
            self.assertEqual(bridge.validate_bind(address), address)
        for address in ("0.0.0.0", "127.0.0.1", "localhost", "8.8.8.8", "172.32.0.1", "::1"):
            with self.assertRaises(ValueError):
                bridge.validate_bind(address)

    def test_random_private_config_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "private" / "bridge.json"
            bridge.initialize_config(path, "192.168.1.20", 1235)
            config = bridge.read_config(path)
            self.assertEqual(config["bind"], "192.168.1.20")
            self.assertGreaterEqual(len(config["token"]), 32)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)
            with self.assertRaises(FileExistsError):
                bridge.initialize_config(path, "192.168.1.20", 1235)
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                bridge.read_config(path)


if __name__ == "__main__":
    unittest.main()
