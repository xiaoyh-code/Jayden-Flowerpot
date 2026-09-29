"""Mock-only tests: no board, camera, local model, real token, or LAN listener."""

from contextlib import contextmanager
from email.message import Message
import http.client
import io
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch
import urllib.error

import webui_server as web

BRIDGE = {"bind": "192.168.1.20", "port": 1235, "token": "synthetic-test-secret-only-never-production"}
JPEG = b"\xff\xd8synthetic-jpeg\xff\xd9"
CAMERA_SETTINGS = {"fps": 20, "flicker_hz": "50", "wb_mode": "auto", "brightness": 1, "saturation": -2}


def wait_job(app, job_id):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        with app.lock:
            state = dict(app.jobs[job_id])
            busy = app.busy
        if state["status"] != "pending" and not busy:
            return state
        time.sleep(0.005)
    raise AssertionError("Mock job did not finish")


@contextmanager
def running_app(transport=None, stream_opener=None):
    with tempfile.TemporaryDirectory() as directory:
        app = web.App(BRIDGE, settings_path=Path(directory) / "private/webui.json", transport=transport or Mock(return_value={}), stream_opener=stream_opener or web.CameraStream)
        static_root = Path(directory) / "assets"
        static_root.mkdir()
        (static_root / "index.html").write_text("<html>Local UI</html>")
        server = web.WebServer(("127.0.0.1", 0), app, static_root)
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        thread.start()
        try:
            yield server, app
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


def request(server, path, *, method="GET", body=None, headers=None):
    connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
    sent_headers = {}
    if method == "POST":
        sent_headers = {"Content-Type": "application/json", "Origin": f"http://127.0.0.1:{server.server_port}", "X-CSRF-Token": server.app.csrf_token}
    if headers:
        for key, value in headers.items():
            if value is None:
                sent_headers.pop(key, None)
            else:
                sent_headers[key] = value
    raw = None if body is None else json.dumps(body).encode()
    try:
        connection.request(method, path, body=raw, headers=sent_headers)
        response = connection.getresponse()
        content = response.read()
        if response.getheader("Content-Type", "").startswith("application/json"):
            content = json.loads(content)
        return response.status, dict(response.getheaders()), content
    finally:
        connection.close()


class SecurityTests(unittest.TestCase):
    def test_strict_host_origin_and_cross_site(self):
        with running_app() as (server, app):
            for headers in ({"Host": "evil.test"}, {"Host": "192.168.1.20:8787"}, {"Origin": "https://evil.test"}, {"Sec-Fetch-Site": "cross-site"}):
                self.assertEqual(request(server, "/api/state", headers=headers)[0], 403)
            status, headers, body = request(server, "/api/state")
            self.assertEqual(status, 200)
            self.assertNotIn(BRIDGE["token"], json.dumps(body))
            self.assertEqual(body["csrf_token"], app.csrf_token)
            self.assertNotIn("Access-Control-Allow-Origin", headers)
            self.assertEqual(headers["Cache-Control"], "no-store")
            self.assertEqual(headers["Cross-Origin-Resource-Policy"], "same-origin")
            self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
            self.assertIn("connect-src 'self'", headers["Content-Security-Policy"])

    def test_mutations_require_origin_and_csrf(self):
        with running_app() as (server, app):
            body = {"action": "capture"}
            for headers in ({"Origin": None}, {"Origin": "null"}, {"Origin": "https://evil.test"}, {"X-CSRF-Token": None}, {"X-CSRF-Token": "wrong"}):
                self.assertEqual(request(server, "/api/action", method="POST", body=body, headers=headers)[0], 403)
            self.assertFalse(app.jobs)

    def test_duplicate_security_headers_are_rejected(self):
        with running_app() as (server, _):
            conn = http.client.HTTPConnection("127.0.0.1", server.server_port)
            conn.putrequest("GET", "/api/state")
            conn.putheader("Host", f"localhost:{server.server_port}")
            conn.endheaders()
            self.assertEqual(conn.getresponse().status, 403)
            conn.close()

    def test_static_files_are_allowlisted(self):
        with running_app() as (server, _):
            self.assertEqual(request(server, "/")[0], 200)
            for path in ("/private/bridge.json", "/../private/bridge.json", "/%2e%2e/private/bridge.json", "/webui_server.py"):
                self.assertEqual(request(server, path)[0], 404)
            self.assertEqual(request(server, "/api/action", method="DELETE")[0], 405)

    def test_bad_or_oversized_json_rejected_before_job(self):
        with running_app() as (server, app):
            self.assertEqual(request(server, "/api/action", method="POST", body={}, headers={"Content-Length": str(web.MAX_JSON_BYTES + 1)})[0], 413)
            self.assertEqual(request(server, "/api/action", method="POST", body=[], headers={})[0], 400)
            self.assertEqual(request(server, "/api/action", method="POST", body={"action": "chat", "prompt": "x" * 4001})[0], 400)
            for prompt in ("x" * 1024, "字" * 342):
                status, _, result = request(server, "/api/action", method="POST", body={"action": "chat", "prompt": prompt})
                self.assertEqual(status, 400)
                self.assertIn("分開幾次", result["error"])
            self.assertFalse(app.jobs)


class JobTests(unittest.TestCase):
    def test_capture_is_explicit_authenticated_and_ram_only(self):
        transport = Mock(return_value=JPEG)
        with running_app(transport) as (server, app):
            # State refresh may only probe status/models/health, never capture.
            request(server, "/api/state")
            time.sleep(0.03)
            self.assertTrue(all("capture" not in call.args[0] for call in transport.call_args_list))
            status, _, result = request(server, "/api/action", method="POST", body={"action": "capture"})
            self.assertEqual(status, 202)
            job_id = result["job_id"]
            self.assertTrue(job_id.isdigit())
            self.assertLessEqual(len(job_id), 18)
            self.assertEqual(wait_job(app, job_id), {"status": "done"})
            capture = [call for call in transport.call_args_list if "/api/capture" in call.args[0]][0]
            self.assertEqual(capture.args[0], "http://192.168.1.25:80/api/capture")
            self.assertEqual(capture.kwargs["token"], BRIDGE["token"])
            self.assertEqual(capture.kwargs["limit"], 256 * 1024)
            self.assertEqual(capture.kwargs["payload"], {})
            status, headers, image = request(server, f"/api/images/{job_id}.jpg")
            self.assertEqual((status, image), (200, JPEG))
            self.assertEqual(headers["Cache-Control"], "no-store")
            self.assertFalse(app.settings_path.exists())

    def test_busy_returns_409_then_slot_recovers(self):
        entered, release = threading.Event(), threading.Event()
        def blocked(url, **kwargs):
            entered.set()
            release.wait(2)
            return JPEG
        with running_app(blocked) as (server, app):
            _, _, first = request(server, "/api/action", method="POST", body={"action": "capture"})
            self.assertTrue(entered.wait(1))
            self.assertEqual(request(server, "/api/action", method="POST", body={"action": "capture"})[0], 409)
            self.assertEqual(request(server, f"/api/jobs/{first['job_id']}")[2], {"status": "pending"})
            self.assertEqual(request(server, "/api/settings", method="POST", body={"board_host": "192.168.1.30"})[0], 409)
            release.set()
            self.assertEqual(wait_job(app, first["job_id"])["status"], "done")

    def test_capture_bounds_and_errors_do_not_leak_exception(self):
        for outcome in (b"not jpeg", b"\xff\xd8" + b"x" * web.MAX_IMAGE_BYTES + b"\xff\xd9", RuntimeError("private-token sensitive-prompt")):
            transport = Mock(side_effect=outcome) if isinstance(outcome, Exception) else Mock(return_value=outcome)
            app = web.App(BRIDGE, transport=transport)
            job_id = app.start_job("capture", "")
            result = wait_job(app, job_id)
            self.assertEqual(result["status"], "error")
            self.assertFalse(app.images)
            self.assertNotIn("private-token", json.dumps(result))
            self.assertFalse(app.busy)

    def test_look_captures_then_uses_fixed_local_model_without_tools(self):
        transport = Mock(side_effect=[JPEG, {"choices": [{"message": {"content": "一個紅色圓形。"}}]}])
        app = web.App(BRIDGE, transport=transport)
        job_id = app.start_job("look", "相片有甚麼？")
        self.assertEqual(wait_job(app, job_id)["status"], "done")
        self.assertIn("/api/capture", transport.call_args_list[0].args[0])
        call = transport.call_args_list[1]
        self.assertEqual(call.args[0], "http://192.168.1.20:1235/v1/chat/completions")
        sent = call.kwargs["payload"]
        self.assertEqual(sent["model"], web.MODEL)
        self.assertEqual(sent["max_tokens"], 256)
        self.assertEqual(sent["reasoning_effort"], "none")
        self.assertNotIn("tools", sent)
        self.assertTrue(sent["messages"][1]["content"][1]["image_url"]["url"].startswith("data:image/jpeg;base64,"))
        self.assertEqual(app.entries[-1]["text"], "一個紅色圓形。")

    def test_chat_routes_via_board_and_polls_same_id(self):
        seen = []
        def transport(url, **kwargs):
            seen.append((url, kwargs))
            if url.endswith("/api/message"):
                return {"id": kwargs["payload"]["id"], "status": "pending"}
            return {"id": url.split("id=")[1], "status": "done", "reply": "板上工具已就緒。"}
        app = web.App(BRIDGE, transport=transport)
        with patch.object(web.time, "sleep", return_value=None):
            job_id = app.start_job("chat", "你好")
            # Synchronize directly instead of patched polling sleep.
            deadline = time.monotonic() + 2
            while app.busy and time.monotonic() < deadline:
                threading.Event().wait(0.001)
        self.assertEqual(app.jobs[job_id]["status"], "done")
        self.assertEqual(seen[0][1]["payload"], {"text": "你好", "id": job_id})
        self.assertEqual(seen[1][0], "http://192.168.1.25:80/api/result?id=" + job_id)
        self.assertTrue(all(call[1]["token"] == BRIDGE["token"] for call in seen))
        self.assertEqual(app.entries[-1]["text"], "板上工具已就緒。")

    def test_images_and_entries_are_bounded(self):
        app = web.App(BRIDGE, transport=Mock(return_value=JPEG))
        first = None
        for _ in range(17):
            job_id = app.start_job("capture", "")
            first = first or job_id
            wait_job(app, job_id)
        self.assertEqual(len(app.images), 4)
        self.assertNotIn(first, app.images)
        self.assertEqual(len(app.entries), 30)
        self.assertTrue(all("image_url" not in entry or entry["image_url"].split("/")[-1][:-4] in app.images for entry in app.entries))


class TelemetryAndSettingsTests(unittest.TestCase):
    def test_state_is_nonblocking_probes_parallel_and_caches(self):
        release = threading.Event()
        seen = []
        def transport(url, **kwargs):
            seen.append((url, kwargs))
            release.wait(1)
            if "/api/status" in url:
                return {"ok": True, "device": "XIAO", "heap_free": 12345, "camera": True, "busy": False}
            if "/v1/models" in url:
                return {"data": [{"id": web.MODEL}]}
            return {"status": "ok"}
        app = web.App(BRIDGE, transport=transport)
        start = time.monotonic()
        app.state()
        self.assertLess(time.monotonic() - start, 0.1)
        deadline = time.monotonic() + 1
        while len(seen) < 3 and time.monotonic() < deadline:
            time.sleep(0.005)
        self.assertEqual(len(seen), 3)
        self.assertTrue(all(call[1]["timeout"] <= 2 for call in seen))
        release.set()
        while app.health_running and time.monotonic() < deadline:
            time.sleep(0.005)
        state = app.state()
        self.assertTrue(state["device"]["connected"])
        self.assertEqual(state["device"]["heap_free"], 12345)
        self.assertTrue(state["model"]["connected"])
        self.assertTrue(state["bridge"]["connected"])
        app.state()
        self.assertEqual(len(seen), 3)
        model_call = [call for call in seen if "/v1/models" in call[0]][0]
        self.assertNotIn("token", model_call[1])

    def test_private_ip_only_and_0600_persistence(self):
        with running_app() as (server, app):
            for value in ("", "localhost", "127.0.0.1", "8.8.8.8", "192.168.1.25:81", None):
                self.assertEqual(request(server, "/api/settings", method="POST", body={"board_host": value})[0], 400)
            status, _, body = request(server, "/api/settings", method="POST", body={"board_host": "192.168.1.30"})
            self.assertEqual(status, 200)
            self.assertEqual(body, {"ok": True, "board_host": "192.168.1.30"})
            self.assertEqual(web.read_settings(app.settings_path), {"board_host": "192.168.1.30"})
            self.assertEqual(app.settings_path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(app.settings_path.parent.stat().st_mode & 0o777, 0o700)


class CameraSettingsTests(unittest.TestCase):
    def test_save_failure_reports_board_ack_and_preserves_saved_override(self):
        for existing in (None, dict(CAMERA_SETTINGS, fps=10)):
            transport = Mock(return_value={"ok": True, "settings": CAMERA_SETTINGS, "telemetry": {"settings_applied": False}})
            with running_app(transport) as (server, app):
                app.board_camera = dict(CAMERA_SETTINGS, fps=10)
                app.desired_camera = existing
                if existing:
                    web.save_settings(app.settings_path, app.board_host, existing)
                with patch.object(web, "save_settings", side_effect=OSError("synthetic disk error")):
                    status, _, result = request(server, "/api/camera/settings", method="POST", body=CAMERA_SETTINGS)
                self.assertEqual(status, 500)
                self.assertIn("已送到裝置", result["error"])
                self.assertIn("未能保存到 Mac", result["error"])
                self.assertEqual(app.board_camera, CAMERA_SETTINGS)
                self.assertEqual(app.camera_telemetry, {"settings_applied": False})
                self.assertEqual(app.desired_camera, existing)
                self.assertFalse(app.busy)
                if existing:
                    self.assertEqual(web.read_settings(app.settings_path)["camera_settings"], existing)
                else:
                    self.assertFalse(app.settings_path.exists())

    def test_telemetry_has_only_known_valid_sensor_values(self):
        value = {"available": True, "valid": True, "settings_applied": False, "sensor_pid": 0x3660, "sensor_name": "OV3660", "nominal_sensor_fps": 24.8, "exposure_lines": 222.5, "detected_hz": 50, "wb_manual": False, "registers": {"0x1234": 255}, "token": BRIDGE["token"], "xclk_hz": "private-secret", "sysclk_hz": True, "hts": -1, "vts": float("nan")}
        clean = web.clean_camera_telemetry(value)
        self.assertEqual(clean, {"available": True, "valid": True, "settings_applied": False, "wb_manual": False, "sensor_pid": 0x3660, "nominal_sensor_fps": 24.8, "exposure_lines": 222.5, "detected_hz": 50, "sensor_name": "OV3660"})
        self.assertNotIn(BRIDGE["token"], json.dumps(clean))

    def test_read_is_authenticated_read_only_and_does_not_persist(self):
        transport = Mock(return_value={"ok": True, "settings": CAMERA_SETTINGS, "telemetry": {}})
        with running_app(transport) as (server, app):
            status, _, body = request(server, "/api/camera/settings")
            self.assertEqual(status, 200)
            self.assertEqual(body["settings"], CAMERA_SETTINGS)
            self.assertIs(body["saved"], False)
            call = transport.call_args
            self.assertEqual(call.args[0], "http://192.168.1.25:80/api/camera/settings")
            self.assertEqual(call.kwargs["token"], BRIDGE["token"])
            self.assertNotIn("method", call.kwargs)
            self.assertFalse(app.settings_path.exists())
            self.assertFalse(app.images)
            self.assertFalse(app.entries)
            self.assertEqual(app.board_camera, CAMERA_SETTINGS)

    def test_save_is_csrf_protected_validated_idle_only_and_private(self):
        transport = Mock(return_value={"ok": True, "settings": CAMERA_SETTINGS, "telemetry": {}})
        with running_app(transport) as (server, app):
            self.assertEqual(request(server, "/api/camera/settings", method="POST", body=CAMERA_SETTINGS, headers={"X-CSRF-Token": None})[0], 403)
            invalid = [dict(CAMERA_SETTINGS, fps=True), dict(CAMERA_SETTINGS, fps=30), dict(CAMERA_SETTINGS, flicker_hz=50), dict(CAMERA_SETTINGS, wb_mode="cloudy"), dict(CAMERA_SETTINGS, brightness=1.5), dict(CAMERA_SETTINGS, saturation=-3), dict(CAMERA_SETTINGS, token="secret"), {"fps": 20}]
            for values in invalid:
                self.assertEqual(request(server, "/api/camera/settings", method="POST", body=values)[0], 400)
            transport.assert_not_called()
            app.busy = True
            self.assertEqual(request(server, "/api/camera/settings", method="POST", body=CAMERA_SETTINGS)[0], 409)
            app.busy = False
            app.start_preview()
            self.assertEqual(request(server, "/api/camera/settings", method="POST", body=CAMERA_SETTINGS)[0], 409)
            app.stop_preview()
            status, _, body = request(server, "/api/camera/settings", method="POST", body=CAMERA_SETTINGS)
            self.assertEqual(status, 200)
            self.assertEqual(body["settings"], CAMERA_SETTINGS)
            self.assertIs(body["saved"], True)
            self.assertEqual(transport.call_args.kwargs["method"], "POST")
            self.assertEqual(transport.call_args.kwargs["payload"], CAMERA_SETTINGS)
            self.assertEqual(web.read_settings(app.settings_path), {"board_host": "192.168.1.25", "camera_settings": CAMERA_SETTINGS})
            self.assertEqual(app.settings_path.stat().st_mode & 0o777, 0o600)
            app.settings("192.168.1.35")
            self.assertEqual(web.read_settings(app.settings_path)["camera_settings"], CAMERA_SETTINGS)

    def test_saved_settings_reapply_only_before_explicit_capture_or_stream(self):
        events = []
        def transport(url, **kwargs):
            events.append((url, kwargs))
            if url.endswith("/api/camera/settings"):
                return {"ok": True, "settings": kwargs.get("payload", dict(CAMERA_SETTINGS, fps=10)), "telemetry": {}}
            if url.endswith("/api/capture"):
                return JPEG
            if url.endswith("/api/status"):
                return {"ok": True, "busy": False, "streaming": False}
            return {"ok": True}
        opener = Mock(return_value=FakeStream())
        app = web.App(BRIDGE, transport=transport, stream_opener=opener, camera_settings=CAMERA_SETTINGS)
        app.health_checked = time.monotonic()
        self.assertEqual(app.state()["camera_settings"], CAMERA_SETTINGS)
        self.assertEqual(events, [])
        read = app.read_camera_settings()
        self.assertEqual(read["settings"]["fps"], 20)
        self.assertEqual(read["board_settings"]["fps"], 10)
        self.assertNotIn("method", events[-1][1])
        events.clear()
        job_id = app.start_job("capture", "")
        self.assertEqual(wait_job(app, job_id)["status"], "done")
        self.assertTrue(events[0][0].endswith("/api/camera/settings"))
        self.assertEqual(events[0][1]["payload"], CAMERA_SETTINGS)
        self.assertTrue(events[1][0].endswith("/api/capture"))
        events.clear()
        app.start_preview()
        self.assertFalse(events)
        opener.assert_not_called()
        preview = app.claim_preview(app.preview.id)
        app.open_preview(preview)
        self.assertTrue(events[0][0].endswith("/api/camera/settings"))
        opener.assert_called_once()
        app.stop_preview()

    def test_failed_settings_application_does_not_capture(self):
        transport = Mock(side_effect=web.Problem("裝置未接受相機設定。"))
        app = web.App(BRIDGE, transport=transport, camera_settings=CAMERA_SETTINGS)
        job_id = app.start_job("capture", "")
        self.assertEqual(wait_job(app, job_id)["status"], "error")
        self.assertEqual(transport.call_count, 1)
        self.assertTrue(transport.call_args.args[0].endswith("/api/camera/settings"))
        self.assertFalse(app.images)
        self.assertFalse(app.busy)


class TransportTests(unittest.TestCase):
    def response(self, raw, content_type="application/json"):
        response = io.BytesIO(raw)
        response.headers = Message()
        response.headers["Content-Type"] = content_type
        return response

    @patch.object(web.urllib.request, "build_opener")
    def test_jpeg_bound_and_no_proxy_or_redirect(self, opener):
        opener.return_value.open.return_value = self.response(JPEG, "image/jpeg")
        self.assertEqual(web.fetch("http://192.168.1.25:80/api/capture", method="POST", payload={}, token=BRIDGE["token"], jpeg=True, limit=256 * 1024), JPEG)
        handlers = opener.call_args.args
        self.assertEqual(handlers[0].proxies, {})
        self.assertIsInstance(handlers[1], web.NoRedirect)
        request = opener.return_value.open.call_args.args[0]
        self.assertEqual(request.get_header("Authorization"), "Bearer " + BRIDGE["token"])
        opener.return_value.open.return_value = self.response(JPEG + b"x" * 100, "image/jpeg")
        with self.assertRaises(web.Problem):
            web.fetch("http://192.168.1.25:80/api/capture", jpeg=True, limit=10)

    @patch.object(web.urllib.request, "build_opener")
    def test_error_body_is_never_exposed(self, opener):
        opener.return_value.open.side_effect = urllib.error.HTTPError("http://192.168.1.25", 500, "private-secret", {}, io.BytesIO(b"private-secret"))
        with self.assertRaises(web.Problem) as raised:
            web.fetch("http://192.168.1.25:80/api/status")
        self.assertNotIn("private", str(raised.exception))


def multipart(frames, close=True):
    parts = [b"--espclawframe\r\nContent-Type: image/jpeg\r\nContent-Length: " + str(len(frame)).encode() + b"\r\n\r\n" + frame + b"\r\n" for frame in frames]
    return b"".join(parts) + (b"--espclawframe--\r\n" if close else b"")


class FakeStream:
    def __init__(self, raw=None):
        self.response = io.BytesIO(raw if raw is not None else multipart([JPEG, JPEG]))
        self.aborted = False
        self.closed = False

    def abort(self):
        self.aborted = True

    def close(self):
        self.closed = True


def stopped_board(url, **kwargs):
    if url.endswith("/api/status"):
        return {"ok": True, "busy": False, "streaming": False}
    return {"ok": True, "stopping": True}


class PreviewTests(unittest.TestCase):
    def test_fps_measures_delivered_frames_and_expires_without_new_frames(self):
        preview = web.Preview("192.168.1.25")
        self.assertEqual(preview.delivered_fps(now=100), 0.0)
        preview.delivered_frame(now=100)
        self.assertEqual(preview.delivered_fps(now=100), 0.0)
        for index in range(1, 11):
            preview.delivered_frame(now=100 + index * 0.1)
        self.assertEqual(preview.frames, 11)
        self.assertEqual(preview.delivered_fps(now=101), 10.0)
        self.assertEqual(preview.delivered_fps(now=102), 5.0)
        self.assertEqual(preview.delivered_fps(now=107), 0.0)
        self.assertEqual(len(preview.frame_times), 0)
        for index in range(200):
            preview.delivered_frame(now=110 + index * 0.01)
        self.assertLessEqual(len(preview.frame_times), 120)
        preview.active = False
        self.assertEqual(preview.delivered_fps(now=112), 0.0)
        fresh = web.Preview("192.168.1.25")
        self.assertEqual(fresh.frames, 0)
        self.assertEqual(fresh.state()["fps"], 0.0)

    def test_parser_validates_lengths_headers_and_jpeg(self):
        self.assertEqual(list(web.mjpeg_frames(io.BytesIO(multipart([JPEG, JPEG])))), [JPEG, JPEG])
        bad_parts = [
            b"--espclawframe\r\nContent-Type: image/jpeg\r\nContent-Length: 262145\r\n\r\n",
            b"--espclawframe\r\nContent-Type: image/jpeg\r\nContent-Length: 8\r\nContent-Length: 8\r\n\r\n",
            b"--espclawframe\r\n" + b"X" * 600 + b"\r\n\r\n",
            multipart([b"not a jpeg"]),
            b"--wrong\r\n",
            multipart([JPEG])[:-30],
        ]
        for raw in bad_parts:
            with self.assertRaises(web.Problem):
                list(web.mjpeg_frames(io.BytesIO(raw)))

    def test_start_requires_csrf_and_never_opens_camera_itself(self):
        transport, opener = Mock(side_effect=stopped_board), Mock(return_value=FakeStream())
        with running_app(transport, opener) as (server, app):
            self.assertEqual(request(server, "/api/stream/start", method="POST", body={}, headers={"X-CSRF-Token": None})[0], 403)
            self.assertIsNone(app.preview)
            status, _, result = request(server, "/api/stream/start", method="POST", body={})
            self.assertEqual(status, 202)
            self.assertRegex(result["stream_url"], r"^/api/stream.mjpg\?session=[a-f0-9]{32}$")
            opener.assert_not_called()
            transport.assert_not_called()
            self.assertTrue(app.preview.starting)
            self.assertFalse(app.images)
            self.assertFalse(app.entries)
            for action in ("capture", "look", "chat"):
                self.assertEqual(request(server, "/api/action", method="POST", body={"action": action, "prompt": "hi"})[0], 409)
            self.assertEqual(request(server, "/api/settings", method="POST", body={"board_host": "192.168.1.40"})[0], 409)
            self.assertEqual(request(server, "/api/stream/stop", method="POST", body={})[2], {"ok": True})
            self.assertFalse(app.preview.active)
            transport.assert_not_called()

    def test_stream_rebuilds_bounded_frames_and_cleans_up(self):
        camera = FakeStream()
        opener, transport = Mock(return_value=camera), Mock(side_effect=stopped_board)
        with running_app(transport, opener) as (server, app):
            _, _, result = request(server, "/api/stream/start", method="POST", body={})
            path = result["stream_url"]
            self.assertEqual(request(server, path, headers={"Origin": "https://evil.test"})[0], 403)
            opener.assert_not_called()
            status, headers, raw = request(server, path)
            self.assertEqual(status, 200)
            self.assertEqual(headers["Content-Type"], "multipart/x-mixed-replace; boundary=espclawframe")
            self.assertEqual(headers["Cache-Control"], "no-store")
            self.assertEqual(headers["Cross-Origin-Resource-Policy"], "same-origin")
            self.assertEqual(list(web.mjpeg_frames(io.BytesIO(raw))), [JPEG, JPEG])
            opener.assert_called_once_with("192.168.1.25", BRIDGE["token"])
            self.assertEqual(app.preview.frames, 2)
            self.assertFalse(app.preview.active)
            self.assertTrue(camera.aborted and camera.closed)
            self.assertFalse(app.images)
            self.assertFalse(app.entries)
            self.assertIsNone(app.latest_image)
            self.assertTrue(any(call.args[0].endswith("/api/stream/stop") for call in transport.call_args_list))
            self.assertTrue(all("1234" not in call.args[0] and "1235" not in call.args[0] for call in transport.call_args_list))
            self.assertEqual(request(server, path)[0], 410)
            self.assertEqual(opener.call_count, 1)

    def test_single_viewer_and_unused_start_expiry(self):
        opener = Mock(return_value=FakeStream())
        app = web.App(BRIDGE, transport=Mock(side_effect=stopped_board), stream_opener=opener)
        path = app.start_preview()["stream_url"]
        preview = app.claim_preview(path.split("=")[1])
        app.open_preview(preview)
        with self.assertRaises(web.Problem) as raised:
            app.claim_preview(preview.id)
        self.assertEqual(raised.exception.status, 409)
        app.stop_preview()
        with patch.object(web, "PREVIEW_LEASE", 0.02):
            transport = Mock()
            app = web.App(BRIDGE, transport=transport, stream_opener=opener)
            app.start_preview()
            deadline = time.monotonic() + 1
            while app.preview.active and time.monotonic() < deadline:
                time.sleep(0.005)
            self.assertFalse(app.preview.active)
            self.assertIn("未開始", app.preview.error)
            transport.assert_not_called()
        self.assertEqual(opener.call_count, 1)

    def test_stop_keeps_jobs_blocked_until_release_confirmed(self):
        releasing = False
        def transport(url, **kwargs):
            if url.endswith("/api/status"):
                return {"ok": True, "streaming": not releasing, "busy": not releasing}
            return {"ok": True}
        app = web.App(BRIDGE, transport=transport, stream_opener=Mock(return_value=FakeStream()))
        app.start_preview()
        preview = app.claim_preview(app.preview.id)
        app.open_preview(preview)
        with patch.object(web, "PREVIEW_STOP_WAIT", 0.01):
            with self.assertRaises(web.Problem) as raised:
                app.stop_preview()
            self.assertEqual(raised.exception.status, 503)
        self.assertTrue(app.preview.active)
        with self.assertRaises(web.Problem):
            app.start_job("chat", "hi")
        releasing = True
        self.assertEqual(app.stop_preview(), {"ok": True})
        self.assertFalse(app.preview.active)

    def test_stop_race_waits_for_open_then_stops_board(self):
        entered, release = threading.Event(), threading.Event()
        def opener(host, token):
            entered.set()
            release.wait(2)
            return FakeStream()
        transport = Mock(side_effect=stopped_board)
        app = web.App(BRIDGE, transport=transport, stream_opener=opener)
        app.start_preview()
        preview = app.claim_preview(app.preview.id)
        def opening():
            try:
                app.open_preview(preview)
            except web.Problem:
                pass
        worker = threading.Thread(target=opening)
        worker.start()
        self.assertTrue(entered.wait(1))
        stopper = threading.Thread(target=app.stop_preview)
        stopper.start()
        time.sleep(0.02)
        transport.assert_not_called()
        self.assertTrue(app.preview.active)
        release.set()
        worker.join(2)
        stopper.join(2)
        self.assertFalse(app.preview.active)
        self.assertTrue(preview.upstream.aborted)

    def test_browser_disconnect_stops_camera(self):
        class SlowReader(io.BytesIO):
            def readline(self, size=-1):
                time.sleep(0.002)
                return super().readline(size)
        camera = FakeStream()
        camera.response = SlowReader(multipart([JPEG] * 1000))
        transport = Mock(side_effect=stopped_board)
        with running_app(transport, Mock(return_value=camera)) as (server, app):
            url = app.start_preview()["stream_url"]
            connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=2)
            connection.request("GET", url)
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            response.read(80)
            response.close()
            connection.close()
            deadline = time.monotonic() + 2
            while app.preview.active and time.monotonic() < deadline:
                time.sleep(0.005)
            self.assertFalse(app.preview.active)
            self.assertLess(app.preview.frames, 1000)
            self.assertTrue(camera.aborted and camera.closed)
            self.assertTrue(any(call.args[0].endswith("/api/stream/stop") for call in transport.call_args_list))

    def test_deadline_and_server_exit_cleanup(self):
        with running_app(Mock(side_effect=stopped_board), Mock(return_value=FakeStream())) as (server, app):
            app.start_preview()
            preview = app.claim_preview(app.preview.id)
            app.open_preview(preview)
            app._expire_preview(preview)
            self.assertFalse(preview.active)
            self.assertIn("10 分鐘", preview.error)
            app.start_preview()
            second = app.claim_preview(app.preview.id)
            app.open_preview(second)
        self.assertFalse(second.active)
        self.assertTrue(second.upstream.aborted)


if __name__ == "__main__":
    unittest.main()
