# /// script
# requires-python = ">=3.10"
# dependencies = [
#   "pyserial",
#   "pillow",
#   "requests",
#   "numpy",
#   "sounddevice",
#   "sherpa-onnx",
#   "pypinyin",
#   "fonttools",
#   "opencc-python-reimplemented",
#   "pynput",
#   "psutil",
# ]
# ///
"""Mac companion for the ESP32 vinyl dashboard (dashboard2).

Feeds the board over USB serial: Now Playing (NetEase) + cover + synced lyrics,
clock, Shenzhen weather, and the 小耄 voice assistant (mic on the Mac).

Run:   uv run host/dashboard.py
Config: ~/.maodie.env (see maodie.env.example). Never put keys in this file.
Protocol: ../PROTOCOL.md
"""
import base64
import fcntl
import glob
import io
import json
import os
import re
import struct
import subprocess
import sys
import tarfile
import threading
import time
import urllib.request
import zlib
from datetime import datetime

import requests
import serial

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser("~")
CACHE = os.path.join(HOME, ".cache", "maodie")
os.makedirs(CACHE, exist_ok=True)


def load_env():
    cfg = {}
    path = os.path.join(HOME, ".maodie.env")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip().strip('"').strip("'")
    return cfg


CFG = load_env()
DEBUG = bool(os.environ.get("MAODIE_DEBUG"))
BAUD = 460800                      # must match LINK_BAUD in main/link.h
ART_N = 240                        # must match ART_N in main/link.h
ART_CHUNK = 1024
PORT = CFG.get("SERIAL_PORT") or (sorted(glob.glob("/dev/cu.usbserial-*")) or ["/dev/cu.usbserial-XXXX"])[0]


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---------------------------------------------------------------- text for the board's font
class Glyphs:
    """Keep only what the board's font can draw: unknown traditional chars go simplified, emoji go away."""

    def __init__(self):
        self.have = None
        self.t2s = None
        try:
            from fontTools.ttLib import TTFont

            self.have = set(TTFont(os.path.join(HERE, "..", "font", "font.ttf")).getBestCmap())
        except Exception as e:
            log("[font] cmap unavailable:", e)
        try:
            import opencc

            self.t2s = opencc.OpenCC("t2s")
        except Exception as e:
            log("[font] opencc unavailable:", e)

    def fix(self, s):
        s = re.sub(r"[\x00-\x1f\x7f]", " ", str(s or "")).strip()
        if not self.have:
            return s
        out = []
        for ch in s:
            if ord(ch) in self.have:
                out.append(ch)
                continue
            alt = self.t2s.convert(ch) if self.t2s else ch
            if alt != ch and all(ord(c) in self.have for c in alt):
                out.append(alt)
            elif ch.isspace():
                out.append(" ")
            # else: drop (emoji etc.)
        return re.sub(r"\s+", " ", "".join(out)).strip()


GLYPHS = Glyphs()


def fit_bytes(s, n):
    b = s.encode("utf-8")
    if len(b) <= n:
        return s
    return b[:n].decode("utf-8", "ignore").rstrip() + "…"


# ---------------------------------------------------------------- serial link
def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def frame(t, payload):
    body = t.encode() + struct.pack("<H", len(payload)) + payload
    return b"\xA5\x5A" + body + struct.pack("<H", crc16(body))


class Link:
    """One writer thread owns the port. State frames are kept as 'latest value' and
    resent every few seconds, so a dropped frame (bad CRC) heals itself."""

    ORDER = "MYWRKS"

    def __init__(self, port):
        self.port = port
        self.ser = None
        self.mu = threading.Lock()
        self.state = {}          # type -> payload bytes
        self.dirty = set()
        self.voice = None
        self.art = None          # (id, bytes)
        self.art_sent = 0
        self.board = {}          # last status from the board
        self.rxbuf = b""
        self.clock_sent = 0
        self.tx_warned = 0
        self.status_t = 0         # when the board last reported
        self.reset_t = 0
        threading.Thread(target=self.run, daemon=True).start()

    def set(self, t, *fields, limit=200):
        txt = "\x1f".join(fit_bytes(GLYPHS.fix(f), limit) for f in fields)
        p = txt.encode("utf-8")[:1100]
        with self.mu:
            if self.state.get(t) != p:
                self.state[t] = p
                self.dirty.add(t)

    def set_voice(self, level):
        self.voice = level

    def set_art(self, raw):
        aid = zlib.crc32(raw) or 1
        with self.mu:
            if not self.art or self.art[0] != aid:
                self.art = (aid, raw)
                self.art_sent = 0

    def _open(self):
        s = serial.Serial()
        s.port = self.port
        s.baudrate = BAUD
        s.dtr = False            # don't reset the board
        s.rts = False
        s.timeout = 0
        s.write_timeout = 5
        s.exclusive = True
        s.open()
        self.ser = s
        self.dirty = set(self.state)
        self.art_sent = 0
        self.clock_sent = 0
        self.status_t = time.time()
        log(f"[link] connected {self.port} @ {BAUD}")

    def _read_status(self):
        data = self.ser.read(4096)
        if not data:
            return
        self.rxbuf = (self.rxbuf + data)[-8192:]
        while b"\n" in self.rxbuf:
            line, self.rxbuf = self.rxbuf.split(b"\n", 1)
            line = line.decode("utf-8", "replace").strip()
            if line.startswith("#S "):
                st = dict(kv.split("=", 1) for kv in line[3:].split() if "=" in kv)
                up = int(st.get("up", 0))
                if up < int(self.board.get("up", 0)):
                    log("[link] board restarted, resending everything")
                    self.dirty = set(self.state)
                    self.clock_sent = 0
                    self.art_sent = 0
                self.board = st
                self.status_t = time.time()
                try:
                    open(os.path.join(CACHE, "board.txt"), "w").write(time.strftime("%H:%M:%S ") + line + "\n")
                except OSError:
                    pass
                if DEBUG:
                    log("[board]", line)
            elif line and DEBUG:
                log("[board log]", line[:160])

    def snapshot(self, path):
        """Fetch the board's frame buffer block by block (CRC checked) and save it as PNG (debug aid)."""
        from PIL import Image

        W, H, BLK = 800, 480, 8192
        total = W * H * 2
        fb = bytearray(total)
        for off in range(0, total, BLK):
            n = min(BLK, total - off)
            for _attempt in range(5):
                self.ser.write(frame("P", f"{off}\x1f{n}".encode()))
                buf, t0 = b"", time.time()
                got = None
                while time.time() - t0 < 2:
                    buf += self.ser.read(65536)
                    i = buf.find(b"#B ")
                    if i >= 0 and b"\n" in buf[i:]:
                        head, rest = buf[i + 3:].split(b"\n", 1)
                        o, ln, crc = map(int, head.split()[:3])
                        if len(rest) >= ln:
                            data = rest[:ln]
                            if o == off and zlib.crc32(data) == crc:
                                got = data
                            break
                if got:
                    fb[off:off + n] = got
                    break
            else:
                log(f"[snap] block {off} failed")
        Image.frombytes("RGB", (W, H), bytes(fb), "raw", "BGR;16").save(path)
        log(f"[snap] saved {path}")

    def run(self):
        last_full = 0
        snap_req = os.path.join(CACHE, "snap.req")
        while True:
            try:
                if self.ser is None:
                    self._open()
                goto_req = os.path.join(CACHE, "goto.req")      # debug: switch board screen
                if os.path.exists(goto_req):
                    n = open(goto_req).read().strip() or "0"
                    os.remove(goto_req)
                    self.ser.write(frame("G", n.encode()))
                if os.path.exists(snap_req):
                    os.remove(snap_req)
                    self.snapshot(os.path.join(CACHE, "snap.png"))
                self._read_status()
                now = time.time()
                # opening the port can leave the board in its ROM download mode (DTR/RTS glitch)
                # or it may have hung: if it stops reporting, give it a clean reset (EN pulse, IO0 high)
                if now - self.status_t > 8 and now - self.reset_t > 30:
                    self.reset_t = now
                    log("[link] board silent, resetting it")
                    self.ser.dtr = False
                    self.ser.rts = True
                    time.sleep(0.15)
                    self.ser.rts = False
                    self.status_t = now
                if now - last_full > 3:
                    last_full = now
                    with self.mu:
                        self.dirty = set(self.state)
                out = []
                if now - self.clock_sent > 3:       # built at send time so it is never stale
                    self.clock_sent = now
                    tz = int(datetime.now().astimezone().utcoffset().total_seconds())
                    out.append(frame("C", f"{int(now)}\x1f{tz}".encode()))
                with self.mu:
                    for t in self.ORDER:
                        if t in self.dirty:
                            out.append(frame(t, self.state[t]))
                    self.dirty.clear()
                    art = self.art
                if self.voice is not None:
                    out.append(frame("V", str(self.voice).encode()))
                    self.voice = None
                if out:
                    self.ser.write(b"".join(out))
                # cover: send whole image, then resend until the board reports it (CRC-checked)
                if art and str(art[0]) != self.board.get("art") and now - self.art_sent > 5:
                    self.art_sent = now
                    aid, raw = art
                    self.ser.write(frame("H", f"{aid}\x1f{zlib.crc32(raw)}".encode()))
                    for off in range(0, len(raw), ART_CHUNK):
                        self.ser.write(frame("A", struct.pack("<II", aid, off) + raw[off:off + ART_CHUNK]))
                    log(f"[link] cover {aid} sent")
                time.sleep(0.03)
            except serial.SerialTimeoutException:
                # host->board TX is stuck (seen when the CH340/cable hangs). Re-opening the port
                # resets the board every time, which flashes the screen, so keep it open and wait.
                if time.time() - self.tx_warned > 60:
                    self.tx_warned = time.time()
                    log("[link] 串口发不出去：USB 线/转接口可能卡住了，拔插一下 USB")
                try:
                    self.ser.reset_output_buffer()
                except Exception:
                    pass
                time.sleep(5)
            except Exception as e:
                log(f"[link] {e}; reconnecting")
                try:
                    self.ser and self.ser.close()
                except Exception:
                    pass
                self.ser = None
                time.sleep(2)


LINK = None


# ---------------------------------------------------------------- now playing
HDR = {"User-Agent": "Mozilla/5.0", "Referer": "https://music.163.com"}

CREDIT_WORDS = ("作词|作曲|词曲|编曲|制作|监制|混音|母带|录音|和声|合音|和音|伴唱|配唱|吉他|贝斯|鼓|键盘|弦乐|钢琴|"
                "人声|出品|发行|统筹|企划|策划|版权|OP|SP|ISRC|词|曲|编|演唱|原唱|翻唱|后期|助理|工程|"
                "Lyrics|Lyricist|Composer|Composed|Arranger|Arranged|Producer|Produced|Mix|Mixed|Mastering|"
                "Mastered|Recording|Recorded|Vocal|Guitar|Bass|Drums|Keyboard|Piano|Strings|Engineer")
CREDIT = re.compile(r"^\s*[^\s:：]{0,12}(" + CREDIT_WORDS + r")[^:：]{0,10}[:：]|^\s*[^\s:：]{1,8}\s*[:：]\s*\S|@|未经.{0,8}许可|不得翻唱",
                    re.I)


def parse_lrc(text):
    out = []
    for line in (text or "").splitlines():
        tags = re.findall(r"\[(\d+):(\d+(?:[.:]\d+)?)\]", line)
        body = re.sub(r"\[[^\]]*\]", "", line).strip()
        if not tags or not body:
            continue
        for m, s in tags:
            out.append((int(m) * 60 + float(s.replace(":", ".")), body))
    out.sort(key=lambda x: x[0])
    out = [x for x in out if not CREDIT.search(x[1])]
    if len(out) <= 1 and out and "纯音乐" in out[0][1]:
        return []
    return out


class Music:
    def __init__(self):
        self.key = None
        self.lyrics = []
        self.lyrics_ready = False
        self.idx = None

    def poll(self):
        try:
            out = subprocess.run(["media-control", "get"], capture_output=True, text=True, timeout=3).stdout
            d = json.loads(out) if out.strip() and out.strip() != "null" else None
        except Exception:
            d = None
        if not d or not d.get("title"):
            LINK.set("M", "没有在播放", "打开网易云音乐放首歌", "", "0", "0", "0")
            LINK.set("Y", "", "", "", "", "")
            self.key = None
            return
        title, artist, album = d.get("title", ""), d.get("artist", ""), d.get("album", "")
        dur = float(d.get("duration") or 0)
        pos = float(d.get("elapsedTime") or 0)
        playing = bool(d.get("playing"))
        if playing and d.get("timestamp"):
            try:
                ts = datetime.fromisoformat(d["timestamp"].replace("Z", "+00:00")).timestamp()
                pos += max(0.0, time.time() - ts) * float(d.get("playbackRate") or 1)
            except Exception:
                pass
        if dur:
            pos = min(pos, dur)

        key = (title, artist)
        if key != self.key:
            self.key = key
            self.lyrics, self.lyrics_ready, self.idx = [], False, None
            log(f"[music] {title} - {artist}")
            threading.Thread(target=self.fetch, args=(key, title, artist, album, dur, d.get("artworkData")),
                             daemon=True).start()

        LINK.set("M", title, artist, album, "1" if playing else "0", str(int(pos * 1000)), str(int(dur * 1000)))
        self.send_lyrics(pos + 0.25)

    def send_lyrics(self, pos):
        L = self.lyrics
        if not L:
            LINK.set("Y", "", "", "纯音乐 / 暂无歌词" if self.lyrics_ready else "歌词加载中…", "", "")
            return
        i = -1
        for k, (t, _) in enumerate(L):
            if t <= pos:
                i = k
            else:
                break
        get = lambda k: L[k][1] if 0 <= k < len(L) else ""
        LINK.set("Y", get(i - 2), get(i - 1), get(i), get(i + 1), get(i + 2))

    # -- lookups (background thread) --
    def fetch(self, key, title, artist, album, dur, artwork_b64):
        song = None
        try:
            r = requests.get("https://music.163.com/api/search/get",
                             params={"s": f"{title} {artist}", "type": 1, "limit": 10}, headers=HDR, timeout=8).json()
            songs = r.get("result", {}).get("songs", []) or []

            def score(sg):
                sc = 0
                if sg.get("name", "").lower() == title.lower():
                    sc += 10
                if any(a.get("name") and a["name"] in artist for a in sg.get("artists", [])):
                    sc += 5
                if dur:
                    sc -= abs(sg.get("duration", 0) / 1000 - dur) / 5
                return sc

            song = max(songs, key=score) if songs else None
        except Exception as e:
            log("[netease search]", e)

        # cover: NetEase high-res, else the Now Playing thumbnail
        img = None
        if song:
            try:
                det = requests.get("https://music.163.com/api/song/detail/",
                                   params={"ids": f"[{song['id']}]"}, headers=HDR, timeout=8).json()
                pic = det["songs"][0]["album"]["picUrl"]
                img = requests.get(pic + "?param=400y400", headers=HDR, timeout=10).content
            except Exception as e:
                log("[cover netease]", e)
        if not img and artwork_b64:
            img = base64.b64decode(artwork_b64)
        if img and self.key == key:
            self.send_cover(img)

        lyr = []
        if song:
            try:
                lr = requests.get("https://music.163.com/api/song/lyric",
                                  params={"id": song["id"], "lv": 1}, headers=HDR, timeout=8).json()
                lyr = parse_lrc(lr.get("lrc", {}).get("lyric", ""))
            except Exception as e:
                log("[lyrics netease]", e)
        if not lyr:
            try:
                r = requests.get("https://lrclib.net/api/get", params={
                    "track_name": title, "artist_name": artist, "album_name": album, "duration": int(dur)}, timeout=8)
                if r.ok:
                    lyr = parse_lrc(r.json().get("syncedLyrics") or "")
            except Exception as e:
                log("[lyrics lrclib]", e)
        if self.key == key:
            self.lyrics, self.lyrics_ready = lyr, True
            log(f"[lyrics] {title}: {len(lyr)} lines")

    def send_cover(self, data):
        try:
            from PIL import Image

            im = Image.open(io.BytesIO(data)).convert("RGB")
            w, h = im.size
            s = min(w, h)
            im = im.crop(((w - s) // 2, (h - s) // 2, (w + s) // 2, (h + s) // 2)).resize((ART_N, ART_N), Image.LANCZOS)
            raw = bytearray()
            for r, g, b in im.get_flattened_data() if hasattr(im, 'get_flattened_data') else im.getdata():
                v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                raw += struct.pack("<H", v)
            LINK.set_art(bytes(raw))
        except Exception as e:
            log("[cover]", e)


# ---------------------------------------------------------------- weather
WEEK = "一二三四五六日"
WMO = {0: "晴", 1: "晴", 2: "多云", 3: "阴", 45: "雾", 48: "雾", 51: "毛毛雨", 53: "毛毛雨", 55: "毛毛雨",
       61: "小雨", 63: "中雨", 65: "大雨", 66: "冻雨", 67: "冻雨", 71: "小雪", 73: "中雪", 75: "大雪",
       80: "阵雨", 81: "阵雨", 82: "暴雨", 95: "雷阵雨", 96: "雷阵雨", 99: "雷阵雨"}


def weather_wttr(city, name):
    d = requests.get(f"https://wttr.in/{city}", params={"format": "j1", "lang": "zh-cn"}, timeout=15).json()
    cur = d["current_condition"][0]
    desc = (cur.get("lang_zh-cn") or [{}])[0].get("value") or cur["weatherDesc"][0]["value"]
    days = d.get("weather", [])
    fc = []
    for w in days[:3]:
        dt = datetime.strptime(w["date"], "%Y-%m-%d")
        hd = w["hourly"][4] if len(w.get("hourly", [])) > 4 else w["hourly"][0]
        dd = (hd.get("lang_zh-cn") or [{}])[0].get("value") or hd["weatherDesc"][0]["value"]
        fc.append(f"周{WEEK[dt.weekday()]}\n{dd}\n{w['mintempC']}~{w['maxtempC']}°")
    today = days[0] if days else {}
    return [name, f"{cur['temp_C']}°", desc, f"{today.get('mintempC', '')}~{today.get('maxtempC', '')}°",
            f"{cur['FeelsLikeC']}°", f"{cur['humidity']}%", f"风速 {cur['windspeedKmph']} km/h", *fc]


def weather_openmeteo(name):
    lat, lon = CFG.get("LAT", "22.54"), CFG.get("LON", "114.06")
    d = requests.get("https://api.open-meteo.com/v1/forecast", timeout=15, params={
        "latitude": lat, "longitude": lon, "timezone": "auto", "forecast_days": 3,
        "current": "temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m",
        "daily": "weather_code,temperature_2m_max,temperature_2m_min"}).json()
    c, dl = d["current"], d["daily"]
    fc = []
    for i in range(3):
        dt = datetime.strptime(dl["time"][i], "%Y-%m-%d")
        fc.append(f"周{WEEK[dt.weekday()]}\n{WMO.get(dl['weather_code'][i], '')}\n"
                  f"{round(dl['temperature_2m_min'][i])}~{round(dl['temperature_2m_max'][i])}°")
    return [name, f"{round(c['temperature_2m'])}°", WMO.get(c["weather_code"], ""),
            f"{round(dl['temperature_2m_min'][0])}~{round(dl['temperature_2m_max'][0])}°",
            f"{round(c['apparent_temperature'])}°", f"{c['relative_humidity_2m']}%",
            f"风速 {round(c['wind_speed_10m'])} km/h", *fc]


def weather_loop():
    city = CFG.get("CITY", "Shenzhen")
    name = CFG.get("CITY_NAME", "深圳")
    while True:
        w = None
        for fn in (lambda: weather_wttr(city, name), lambda: weather_openmeteo(name)):
            try:
                w = fn()
                break
            except Exception as e:
                log("[weather]", e)
        if w:
            LINK.set("W", *w)
            time.sleep(15 * 60)
        else:
            time.sleep(60)


# ---------------------------------------------------------------- keyboard page
class Keys:
    """Global key listener (needs Input Monitoring permission for the Python that runs this).
    Password fields use macOS secure input, so they never reach us."""

    NAMES = {"space": "Space", "enter": "Return", "backspace": "Delete", "tab": "Tab", "esc": "Esc",
             "shift": "Shift", "shift_r": "Shift", "ctrl": "Ctrl", "ctrl_r": "Ctrl", "alt": "Option",
             "alt_r": "Option", "cmd": "Cmd", "cmd_r": "Cmd", "caps_lock": "Caps Lock", "up": "↑", "down": "↓",
             "left": "←", "right": "→", "delete": "Del", "home": "Home", "end": "End", "page_up": "Page Up",
             "page_down": "Page Down", "fn": "Fn"}
    MODS = {"cmd": "Cmd", "cmd_r": "Cmd", "ctrl": "Ctrl", "ctrl_r": "Ctrl", "alt": "Option", "alt_r": "Option",
            "shift": "Shift", "shift_r": "Shift"}

    def __init__(self):
        self.name, self.code, self.stream = "", "", ""
        self.path = os.path.join(CACHE, "keys.json")
        self.count = 0
        self.day = datetime.now().date()
        try:                                     # today's count survives restarts
            d = json.load(open(self.path))
            if d.get("day") == str(self.day):
                self.count = int(d.get("count", 0))
        except Exception:
            pass
        self.times = []
        self.held = set()

    def on_press(self, key):
        n = getattr(key, "name", None)
        vk = getattr(key, "vk", None)
        if vk is None and getattr(key, "value", None) is not None:
            vk = getattr(key.value, "vk", None)
        if n in self.MODS:
            self.held.add(self.MODS[n])
            name = self.MODS[n]
        else:
            ch = getattr(key, "char", None)
            if ch and ch.isprintable() and not n:
                name = ch.upper() if len(ch) == 1 else ch
                if not (self.held - {"Shift"}):
                    self.stream += ch
            elif n and n.startswith("f") and n[1:].isdigit():
                name = n.upper()
            else:
                name = self.NAMES.get(n or "", (n or "?").capitalize())
                if n == "space":
                    self.stream += " "
                elif n == "enter":
                    self.stream += " · "
                elif n in ("backspace", "delete") and self.stream:
                    self.stream = self.stream[:-1]
            mods = [m for m in ("Cmd", "Ctrl", "Option", "Shift") if m in self.held]
            if mods and not (mods == ["Shift"] and len(name) == 1):
                name = " + ".join(mods + [name])
        self.stream = self.stream[-40:]
        self.name = name
        self.code = f"键值 {vk}  ·  0x{vk:02X}" if isinstance(vk, int) else ""
        today = datetime.now().date()
        if today != self.day:
            self.day, self.count = today, 0
        self.count += 1
        if self.count == 1:
            log("[keys] first key press seen")
        if self.count % 20 == 0:
            try:
                json.dump({"day": str(self.day), "count": self.count}, open(self.path, "w"))
            except Exception:
                pass
        self.times.append(time.time())
        self.push()

    def on_release(self, key):
        n = getattr(key, "name", None)
        if n in self.MODS:
            self.held.discard(self.MODS[n])

    def kpm(self):
        now = time.time()
        self.times = [t for t in self.times if now - t <= 60]
        return len(self.times)

    def push(self):
        LINK.set("K", self.name, self.code, self.stream, str(self.count), str(self.kpm()))

    def start(self):
        try:
            from pynput import keyboard

            keyboard.Listener(on_press=self.on_press, on_release=self.on_release).start()
            log("[keys] listening (grant Input Monitoring if nothing shows up)")
        except Exception as e:
            log("[keys] listener failed:", e)


# ---------------------------------------------------------------- system page
CLAUDE_USAGE = os.path.join(CACHE, "claude_usage.json")   # written by host/claude_statusline.py


def fmt_rate(bps):
    if bps >= 1e6:
        return f"{bps / 1e6:.1f} MB/s"
    if bps >= 1e3:
        return f"{bps / 1e3:.0f} KB/s"
    return f"{bps:.0f} B/s"


def fmt_reset(ts):
    if not ts:
        return ""
    dt = datetime.fromtimestamp(ts)
    return f"周{WEEK[dt.weekday()]} {dt:%H:%M} 重置"


def weekly_left(used, resets_at):
    """(remaining %, reset text) from a used % and reset time; a passed reset means a fresh week."""
    if used is None:
        return "", ""
    if resets_at and resets_at < time.time():
        return "100", "已重置"
    return str(max(0, round(100 - float(used)))), fmt_reset(resets_at)


def five_hour_text(used, resets_at):
    left, _ = weekly_left(used, resets_at)
    return f"5小时额度 剩{left}%" if left else ""


def codex_weekly():
    """Codex CLI logs its rate limits into ~/.codex/sessions/**/rollout-*.jsonl; take the newest.
    Returns (weekly remaining %, 5-hour remaining text)."""
    files = sorted(glob.glob(os.path.join(HOME, ".codex", "sessions", "*", "*", "*", "*.jsonl")),
                   key=os.path.getmtime, reverse=True)[:5]
    for f in files:
        try:
            with open(f, "rb") as fh:
                fh.seek(max(0, os.path.getsize(f) - 400_000))
                tail = fh.read().decode("utf-8", "ignore")
        except OSError:
            continue
        for m in reversed(re.findall(r'"rate_limits":(\{.*?"secondary":\{[^}]*\})', tail)):
            try:
                pri = json.loads("{" + re.search(r'"primary":\{([^}]*)\}', m).group(1) + "}")
                sec = json.loads("{" + re.search(r'"secondary":\{([^}]*)\}', m).group(1) + "}")
            except (ValueError, AttributeError):
                continue
            return (weekly_left(sec.get("used_percent"), sec.get("resets_at"))[0],
                    five_hour_text(pri.get("used_percent"), pri.get("resets_at")))
    return "", ""


def claude_weekly():
    try:
        rl = json.load(open(CLAUDE_USAGE)).get("rate_limits") or {}
        w, h = rl.get("seven_day") or {}, rl.get("five_hour") or {}
        return (weekly_left(w.get("used_percentage"), w.get("resets_at"))[0],
                five_hour_text(h.get("used_percentage"), h.get("resets_at")))
    except Exception:
        return "", ""


def gpu_percent():
    try:
        out = subprocess.run(["/usr/sbin/ioreg", "-r", "-d", "1", "-c", "IOAccelerator"],
                             capture_output=True, text=True, timeout=3).stdout
        m = re.search(r'"Device Utilization %"=(\d+)', out)
        return m.group(1) if m else ""
    except Exception:
        return ""


def system_loop():
    import psutil

    psutil.cpu_percent(None)
    last = psutil.net_io_counters()
    last_t = time.time()
    usage, usage_t = ("", "", "", ""), 0
    while True:
        time.sleep(1)
        now = time.time()
        net = psutil.net_io_counters()
        dt = max(0.1, now - last_t)
        down = (net.bytes_recv - last.bytes_recv) / dt
        up = (net.bytes_sent - last.bytes_sent) / dt
        last, last_t = net, now
        vm = psutil.virtual_memory()
        if now - usage_t > 30:
            usage, usage_t = (*codex_weekly(), *claude_weekly()), now
        LINK.set("S", str(round(psutil.cpu_percent(None))), gpu_percent(), str(round(vm.percent)),
                 f"{(vm.total - vm.available) / 2**30:.1f} / {vm.total / 2**30:.0f} GB",
                 fmt_rate(down), fmt_rate(up), *usage)


# ---------------------------------------------------------------- 小耄
SYSTEM_PROMPT = CFG.get("BOT_PROMPT") or (
    "你是「小耄」，一个住在桌面小屏幕里的猫猫 AI 助手，原型是网络上爱哈气的圆头橘猫耄耋。"
    "用口语化的中文回答，简短有趣，一般不超过 60 个字，不要用 markdown 和表情符号。"
    "每次回复最前面加一个情绪标签，只能是 [开心] [难过] [生气] [惊讶] [平静] [调皮] 之一，然后再写回复。")
EMOTIONS = {"开心": "happy", "难过": "sad", "生气": "angry", "惊讶": "surprised", "平静": "calm", "调皮": "playful"}
BOT = {"state": "idle", "mood": "calm", "user": "", "reply": "说「你好耄耋」叫我"}


def bot_set(state, user=None, reply=None):
    BOT["state"] = state
    if user is not None:
        BOT["user"] = user
    if reply is not None:
        BOT["reply"] = reply
    LINK.set("R", BOT["state"], BOT["mood"], BOT["user"], BOT["reply"], limit=600)


def split_emotion(reply):
    m = re.match(r"\s*[\[【]([^\]】]{1,4})[\]】]\s*", reply)
    if not m:
        return "calm", reply
    return EMOTIONS.get(m.group(1), "calm"), reply[m.end():]


def ask_llm(history):
    provider = CFG.get("LLM_PROVIDER", "anthropic").lower()
    key = CFG.get("LLM_API_KEY", "")
    model = CFG.get("LLM_MODEL", "")
    if not key:
        return "[难过] 还没配置 API，在 ~/.maodie.env 里填 LLM_API_KEY。"
    if provider == "anthropic":
        base = (CFG.get("LLM_BASE_URL") or "https://api.anthropic.com").rstrip("/")
        r = requests.post(f"{base}/v1/messages", timeout=60, headers={
            "x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json"},
            json={"model": model or "claude-haiku-5-5", "max_tokens": 300, "system": SYSTEM_PROMPT,
                  "messages": history})
        r.raise_for_status()
        return "".join(b.get("text", "") for b in r.json().get("content", []))
    base = (CFG.get("LLM_BASE_URL") or "https://api.openai.com/v1").rstrip("/")
    r = requests.post(f"{base}/chat/completions", timeout=60, headers={"Authorization": f"Bearer {key}"},
                      json={"model": model, "messages": [{"role": "system", "content": SYSTEM_PROMPT}] + history,
                            "max_tokens": 300})
    r.raise_for_status()
    return r.json()["choices"][0]["message"]["content"]


def download(url, dest_dir):
    path = os.path.join(dest_dir, url.rsplit("/", 1)[1])
    if not os.path.exists(path):
        log(f"[bot] downloading {os.path.basename(path)} …")
        urllib.request.urlretrieve(url, path + ".part")
        os.rename(path + ".part", path)
    return path


def ensure_models():
    rel = "https://github.com/k2-fsa/sherpa-onnx/releases/download"
    vad = download(f"{rel}/asr-models/silero_vad.onnx", CACHE)
    asr_dir = os.path.join(CACHE, "sherpa-onnx-paraformer-zh-small-2024-03-09")
    if not os.path.isdir(asr_dir):
        tb = download(f"{rel}/asr-models/sherpa-onnx-paraformer-zh-small-2024-03-09.tar.bz2", CACHE)
        with tarfile.open(tb) as t:
            t.extractall(CACHE, filter="data")
    return vad, asr_dir


WAKE_SYL = [("ni",), ("hao",), ("mao", "mo", "miao", "man"), ("die", "de", "dei", "tie", "di", "jie")]


def find_wake(text):
    """Index (in chars) right after a fuzzy "你好耄耋", or -1."""
    from pypinyin import lazy_pinyin

    py = lazy_pinyin(list(text))
    for i in range(len(py) - 3):
        if all(py[i + k] in WAKE_SYL[k] for k in range(4)):
            return i + 4
    for i in range(min(3, len(py) - 1)):          # bare "耄耋" at the start
        if py[i] in WAKE_SYL[2] and py[i + 1] in WAKE_SYL[3]:
            return i + 2
    return -1


def robot_loop():
    try:
        import numpy as np
        import sherpa_onnx
        import sounddevice as sd
    except Exception as e:
        log("[bot] disabled:", e)
        return
    try:
        vad_path, asr_dir = ensure_models()
    except Exception as e:
        log("[bot] model download failed:", e)
        bot_set("idle", "", "语音模型下载失败，检查网络后重启脚本")
        return
    rec = sherpa_onnx.OfflineRecognizer.from_paraformer(
        paraformer=os.path.join(asr_dir, "model.int8.onnx"), tokens=os.path.join(asr_dir, "tokens.txt"),
        num_threads=2, sample_rate=16000, feature_dim=80, decoding_method="greedy_search")
    vcfg = sherpa_onnx.VadModelConfig()
    vcfg.silero_vad.model = vad_path
    vcfg.silero_vad.min_silence_duration = 0.5
    vcfg.silero_vad.min_speech_duration = 0.25
    vcfg.silero_vad.max_speech_duration = 15
    vcfg.sample_rate = 16000
    vad = sherpa_onnx.VoiceActivityDetector(vcfg, buffer_size_in_seconds=60)

    history = []
    awake_until = [0.0]
    busy = threading.Event()
    speaking = [None]
    voice = CFG.get("TTS_VOICE", "Tingting")
    dev = CFG.get("MIC_DEVICE") or None
    log(f"[bot] listening on mic: {dev or 'default'}")
    bot_set("idle")

    def answer(question):
        nonlocal history
        busy.set()
        try:
            bot_set("think", question, "")
            history = (history + [{"role": "user", "content": question}])[-12:]
            try:
                mood, reply = split_emotion(ask_llm(history).strip())
            except Exception as e:
                mood, reply = "sad", f"出错了：{type(e).__name__}"
                log("[bot] llm error:", e)
            BOT["mood"] = mood
            history.append({"role": "assistant", "content": reply})
            bot_set("speak", question, reply)
            speaking[0] = subprocess.Popen(["say", "-v", voice, reply])
            speaking[0].wait()
            interrupted = speaking[0].returncode not in (0, None)
            speaking[0] = None
            if not interrupted:
                # keep listening a few seconds for a follow-up, no wake word needed
                awake_until[0] = time.time() + 8
                bot_set("listen")
        finally:
            busy.clear()

    frames = 0
    with sd.InputStream(samplerate=16000, channels=1, dtype="float32", blocksize=512, device=dev) as mic:
        while True:
            data, _ = mic.read(512)
            frames += 1
            if frames % 3 == 0:          # ~10 Hz level for the line
                rms = float(np.sqrt(np.mean(data[:, 0] ** 2)))
                LINK.set_voice(min(100, int(rms * 600)))
            vad.accept_waveform(data[:, 0].copy())
            while not vad.empty():
                seg = np.array(vad.front.samples, dtype=np.float32)
                vad.pop()
                st = rec.create_stream()
                st.accept_waveform(16000, seg)
                rec.decode_stream(st)
                text = st.result.text.strip()
                if not text:
                    continue
                now = time.time()
                w = find_wake(text)
                log(f"[bot] heard: {text}  wake={w}")
                if busy.is_set():
                    # while thinking/talking only the wake word counts: it interrupts
                    if w >= 0 and speaking[0]:
                        speaking[0].terminate()
                        awake_until[0] = now + 8
                        bot_set("listen")
                    continue
                if w >= 0:
                    rest = text[w:].strip(" ，,。.!！?？")
                    if len(rest) >= 2:
                        threading.Thread(target=answer, args=(rest,), daemon=True).start()
                    else:
                        awake_until[0] = now + 8
                        bot_set("listen")
                elif now < awake_until[0]:
                    awake_until[0] = 0
                    threading.Thread(target=answer, args=(text,), daemon=True).start()
            if awake_until[0] and time.time() > awake_until[0] and not busy.is_set():
                awake_until[0] = 0
                bot_set("idle")


# ---------------------------------------------------------------- main
def single_instance():
    f = open(os.path.join(CACHE, "dashboard.lock"), "w")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        log("[main] 另一个 dashboard.py 已经在运行（开机自启的那个），这个退出。")
        time.sleep(30)
        sys.exit(0)
    return f


def main():
    global LINK
    _lock = single_instance()
    LINK = Link(PORT)
    bot_set("idle")
    music = Music()
    keys = Keys()
    keys.start()
    keys.push()
    threading.Thread(target=weather_loop, daemon=True).start()
    threading.Thread(target=system_loop, daemon=True).start()
    if not os.environ.get("MAODIE_NO_BOT"):
        threading.Thread(target=robot_loop, daemon=True).start()
    while True:
        music.poll()
        keys.push()          # keeps the per-minute rate falling when you stop typing
        time.sleep(0.4)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
