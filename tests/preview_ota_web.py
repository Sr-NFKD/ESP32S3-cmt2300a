"""Read-only local preview of the embedded OTA page; never flashes a device."""
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import json

root = Path(__file__).resolve().parents[1]

class Preview(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/info":
            data = json.dumps({"chip": "ESP32-S3", "version": "本地样式预览",
                               "project": "ESP32S3_A07_CC1101", "max_size": 0x1e0000}).encode()
            content_type = "application/json"
        elif self.path == "/api/signals":
            # Preview fixtures only; the firmware reads its actual NVS library.
            data = json.dumps([
                {"id": 1, "name": "客厅灯（预览）", "key_code": "00AB12CD", "frequency_hz": 433920000},
                {"id": 2, "name": "车库门（预览）", "key_code": "00123456", "frequency_hz": 315000000}
            ]).encode()
            content_type = "application/json"
        elif self.path == "/":
            data = (root / "components/debug_ota/web/index.html").read_bytes()
            content_type = "text/html; charset=utf-8"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        self.send_error(405, "Local preview only; connect to the device for OTA")

if __name__ == "__main__":
    print("Read-only OTA page preview: http://127.0.0.1:8765", flush=True)
    HTTPServer(("127.0.0.1", 8765), Preview).serve_forever()
