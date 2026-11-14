"""Serve the phone setup page locally with a fake API, for UI testing.

usage: python tools/mock_setup_page.py   ->  http://localhost:8765
"""
import json, os, re
from http.server import BaseHTTPRequestHandler, HTTPServer

src = open(os.path.join(os.path.dirname(__file__), "..", "WebSetup.h"), encoding="utf-8").read()
PAGE = re.search(r'R"HTML\((.*)\)HTML"', src, re.S).group(1).encode()
stats_src = open(os.path.join(os.path.dirname(__file__), "..", "StatsPage.h"), encoding="utf-8").read()
STATS = re.search(r'R"HTML\((.*)\)HTML"', stats_src, re.S).group(1).encode()


def fake_history():
    """Five weeks of plausible Sorento driving."""
    import random, time
    random.seed(4)
    now, drives, odo = int(time.time()), [], 151.0
    for i in range(60):
        t = now - (60 - i) * 14 * 3600 + random.randint(-3000, 3000)
        hwy = random.random() < 0.3
        mi = random.uniform(12, 45) if hwy else random.uniform(1.5, 9)
        mpg = random.uniform(19, 23) if hwy else random.uniform(13, 18)
        sec = int(mi / (random.uniform(48, 62) if hwy else random.uniform(18, 30)) * 3600)
        idle = int(sec * random.uniform(0.05, 0.25))
        drives.append([t, round(odo, 1), round(mi, 2), round(mi / mpg, 3), sec, idle, round(idle / 3600 * 0.6, 3), random.randint(40, 75)])
        odo += mi
    fills = [[now - 26 * 86400, 420.0, 17.8, 16.9, 318.0, 1], [now - 14 * 86400, 730.0, 18.6, 18.1, 331.0, 1], [now - 3 * 86400, 1050.0, 18.1, 17.9, 322.0, 1]]
    bands = {"min": [1, 15, 25, 35, 45, 55, 65, 75], "mi": [22, 64, 120, 140, 110, 160, 230, 14],
             "gal": [2.3, 5.1, 8.6, 8.9, 6.1, 7.9, 11.8, 0.8], "sec": [9000, 11000, 14000, 12000, 8000, 9500, 12000, 600],
             "idleSec": 30000, "idleGal": 4.8}
    return {"odoOffset": 151890.0, "now": 0, "storage": True, "drives": drives, "fills": fills, "bands": bands}

STATE = {"obd": {"state": "Connected", "adapter": "IOS-Vlink", "sim": False, "readOnly": True, "mpg": 18.2, "gph": 0.6,
                 "mph": 31, "coolant": 196, "volts": 14.1, "trip": {"mi": 151.1, "gal": 6.96, "sec": 15000, "mpg": 21.7},
                 "tank": {"mi": 22.1, "gal": 1.09, "sec": 2000, "mpg": 20.3}, "range": 406, "disp": 3.5, "tankCap": 21.1,
                 "cal": 1.0, "tankFull": True, "tankIncomplete": False, "calFills": 0, "lastFill": 2, "lastFillEst": 5.9,
                 "lastFillPumped": 16.2, "mil": False, "codesRead": True, "codes": [],
                 "trans": "2 codes", "transBusy": False, "transCodes": [
                     {"code": "P0715", "now": True, "text": "Input/turbine speed sensor"},
                     {"code": "P0741", "now": False, "text": "Lock-up clutch stuck off"}]},
         "connected": True, "volume": 80, "brightness": 90, "leds": True, "tint": 1, "fw": "mock", "dimAfter": 60, "dimPct": 20,
         "pages": [{"name": n, "hidden": h} for n, h in [("Economy", False), ("Trip & range", False), ("Engine", False),
                   ("Trouble codes", False), ("Engine health", True), ("Service", True), ("History", False)]],
         "maint": {"odometer": 181266, "speedCal": 0.0, "sinceSet": 120, "items": [{"name": n, "interval": i, "last": None, "remaining": None} for n, i in
                   [("Oil change", 5000), ("Tire rotation", 7500), ("Engine air filter", 15000), ("Cabin air filter", 15000)]]},
         "horns": [
    {"key": f"T{n:02d}     WAV", "file": f"T{n:02d}.WAV", "auto": f"Horn {i + 1}", "name": "", "hidden": False}
    for i, n in enumerate([2, 4, 6, 8, 10])]}

class H(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="text/plain"):
        self.send_response(code); self.send_header("Content-Type", ctype); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        if self.path == "/api/state": self._send(200, json.dumps(STATE).encode(), "application/json")
        elif self.path == "/api/history": self._send(200, json.dumps(fake_history()).encode(), "application/json")
        elif self.path == "/stats": self._send(200, STATS, "text/html")
        elif self.path == "/api/health": self._send(200, json.dumps({"drives": 9, "learnLeft": 0, "checks": [
            {"name": "Air leaks", "status": "watch", "summary": "lean at idle +14%", "detail": "Fuel trim is high at idle and normal on the highway: a vacuum leak is the usual cause."},
            {"name": "Battery & charging", "status": "ok", "summary": "14.1 V charging", "detail": "Resting and charging voltage look normal."},
            {"name": "Thermostat", "status": "ok", "summary": "warms up normally", "detail": "Reaches operating temperature in the expected time."},
            {"name": "Knock / timing", "status": "learning", "summary": "needs more drives", "detail": "Still learning this car's normal."}]}).encode(), "application/json")
        elif self.path == "/api/log": self._send(200, b"Sorento Dash mock, restart #12, up 3 min\n#12 0:00:00  Started vMock: power on\n#12 0:00:07  Adapter connected: IOS-Vlink\n#12 0:00:13  Car on: engine computer answering\n")
        else: self._send(200, PAGE, "text/html")
    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(n) or b"{}")
        print(self.path, body)
        if self.path == "/api/horns":
            by_key = {h["key"]: h for h in STATE["horns"]}
            STATE["horns"] = [{**by_key[h["key"]], "name": h["name"], "hidden": h["hidden"]} for h in body["horns"]]
        elif self.path == "/api/maint" and "items" in body:
            STATE["maint"]["items"] = [{**it, "remaining": None} for it in body["items"]]
        elif self.path == "/api/settings":
            STATE.update({k: v for k, v in body.items() if k in STATE})
        self._send(200, b"ok")

from http.server import ThreadingHTTPServer
ThreadingHTTPServer(("localhost", 8765), H).serve_forever()
