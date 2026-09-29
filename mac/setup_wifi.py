#!/usr/bin/env python3
"""Local-only Wi-Fi credential form; never prints or uploads credentials."""
import argparse
import html
import json
import os
from pathlib import Path
import secrets
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs

PRIVATE = Path(__file__).resolve().parent / "private"


def validate(ssid, password):
    if not 1 <= len(ssid.encode("utf-8")) <= 32 or "\x00" in ssid:
        raise ValueError("Wi-Fi 名稱必須為 1–32 bytes。")
    if "\x00" in password or (password and not 8 <= len(password.encode("utf-8")) <= 63):
        raise ValueError("WPA/WPA2 密碼須為 8–63 bytes；開放網絡請留空。")


def save_wifi(ssid, password):
    validate(ssid, password)
    PRIVATE.mkdir(mode=0o700, parents=True, exist_ok=True)
    PRIVATE.chmod(0o700)
    path = PRIVATE / "wifi.json"
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump({"ssid": ssid, "password": password}, f)


class SetupHandler(BaseHTTPRequestHandler):
    nonce = secrets.token_urlsafe(32)

    def log_message(self, *_):
        pass

    def send_page(self, body, status=200):
        page = ("<!doctype html><html lang='zh-Hant'><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>ESPClaw Wi-Fi 設定</title><style>"
                "body{font:17px system-ui;background:#f2f4f0;color:#20332a;margin:0;padding:40px 20px}"
                "main{max-width:540px;margin:5vh auto;background:white;padding:32px;border-radius:18px}"
                "h1{font-size:28px}p{line-height:1.6}label{display:block;margin:22px 0 8px}"
                "input{box-sizing:border-box;width:100%;font:inherit;padding:12px;border:1px solid #8a9a90;border-radius:8px}"
                "button{margin-top:24px;background:#216348;color:white;border:0;padding:14px 22px;"
                "border-radius:8px;font:inherit;cursor:pointer}small{display:block;line-height:1.6;color:#536359}"
                "</style><main>" + body + "</main></html>").encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(page)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(page)

    def host_ok(self):
        return self.headers.get("Host") == f"127.0.0.1:{self.server.server_port}"

    def do_GET(self):
        if not self.host_ok() or self.path != "/":
            self.send_error(404)
            return
        self.send_page("<small>XIAO ESP32S3 SENSE · 本機設定</small><h1>連接 Wi-Fi</h1>"
            "<p>輸入可與這部 Mac 互通的 <b>2.4 GHz Wi-Fi</b>。密碼只會儲存在這部 Mac，再經 USB 寫入 ESP32。</p>"
            "<form method='post' action='/save'>"
            f"<input type='hidden' name='nonce' value='{self.nonce}'>"
            "<label for='ssid'>Wi-Fi 名稱</label><input id='ssid' name='ssid' required autocomplete='off'>"
            "<label for='password'>Wi-Fi 密碼</label><input id='password' name='password' type='password' autocomplete='off'>"
            "<button type='submit'>儲存並繼續安裝</button></form>"
            "<p><small>唔需要喺 Codex 對話貼出密碼。儲存後可關閉此頁。</small></p>")

    def do_POST(self):
        origin = self.headers.get("Origin")
        if not self.host_ok() or self.path != "/save" or (origin and origin != f"http://127.0.0.1:{self.server.server_port}"):
            self.send_error(403)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 4096:
                raise ValueError("無效的表格。")
            data = parse_qs(self.rfile.read(length).decode(), keep_blank_values=True, strict_parsing=True)
            if not secrets.compare_digest(data.get("nonce", [""])[0], self.nonce):
                self.send_error(403)
                return
            save_wifi(data.get("ssid", [""])[0], data.get("password", [""])[0])
        except (ValueError, UnicodeError) as exc:
            self.send_page("<h1>請檢查輸入</h1><p>" + html.escape(str(exc)) + "</p><a href='/'>返回</a>", 400)
            return
        print("Wi-Fi credentials saved locally; ready for provisioning.", flush=True)
        self.send_page("<h1>已儲存 ✓</h1><p>Wi-Fi 設定已留喺本機，安裝可以繼續。你可以關閉此頁，返回 Codex。</p>")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args()
    server = HTTPServer(("127.0.0.1", args.port), SetupHandler)
    print(f"Wi-Fi setup: http://127.0.0.1:{server.server_port}/", flush=True)
    server.serve_forever()
