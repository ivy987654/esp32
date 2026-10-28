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
# ]
# ///
"""Mac companion for the ESP32 vinyl dashboard.

Feeds the board over USB serial with: NetEase/any Now Playing track + album art,
synced lyrics, weather, clock, keyboard activity and the 小耄 voice robot.

Run:  uv run ~/esp32/dashboard/host/dashboard.py
Config (optional): ~/.maodie.env, see README in this folder.
"""
import base64
import glob
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import threading
import time
import urllib.request
from collections import deque
from datetime import datetime

import requests
import serial

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
PORT = CFG.get("SERIAL_PORT") or (sorted(glob.glob("/dev/cu.usbserial-*")) or ["/dev/cu.usbserial-XXXXXX"])[0]


def clean(s):
    return str(s or "").replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()


# ---------------------------------------------------------------- serial link
class Link:
    def __init__(self, port):
        self.port = port
        self.lock = threading.Lock()
        self.ser = None
        self.last = {}

    def _open(self):
        s = serial.Serial()
        s.port = self.port
        s.baudrate = 115200
        s.dtr = False
        s.rts = False
        s.timeout = 0
        s.write_timeout = 5
        s.exclusive = True
        s.open()
        self.ser = s
        self.last = {}
        print(f"[link] connected {self.port}")

    def send(self, *fields, force=False):
        line = "\t".join(clean(f) if i else f for i, f in enumerate(fields))
        key = fields[0]
        if not force and self.last.get(key) == line:
            return
        with self.lock:
            try:
                if self.ser is None:
                    self._open()
                payload = line.encode("utf-8")
                self.ser.write(payload + b"\t#" + str(len(payload)).encode() + b"\n")
                self.ser.read(4096)  # drain board logs
                self.last[key] = line
            except Exception as e:
                print(f"[link] {e}; retrying")
                try:
                    self.ser and self.ser.close()
                except Exception:
                    pass
                self.ser = None
                time.sleep(1)


LINK = Link(PORT)


# ---------------------------------------------------------------- now playing
class Music:
    def __init__(self):
        self.track_key = None
        self.lyrics = []  # [(t, text)]
        self.lyric_for = None
        self.art = ""

    def poll(self):
        try:
            out = subprocess.run(["media-control", "get"], capture_output=True, text=True, timeout=3).stdout
            d = json.loads(out) if out.strip() and out.strip() != "null" else None
        except Exception:
            d = None
        if not d or not d.get("title"):
            LINK.send("M", "没有在播放", "打开网易云音乐放首歌", "", "0", "0", "0")
            LINK.send("Y", "", "", "", "", "")
            return
        title, artist, album = d.get("title", ""), d.get("artist", ""), d.get("album", "")
        dur = float(d.get("duration") or 0)
        pos = float(d.get("elapsedTime") or 0)
        playing = bool(d.get("playing"))
        if playing and d.get("timestamp"):
            try:
                ts = datetime.fromisoformat(d["timestamp"].replace("Z", "+00:00")).timestamp()
                pos += max(0, time.time() - ts) * float(d.get("playbackRate") or 1)
            except Exception:
                pass
        pos = min(pos, dur) if dur else pos

        key = (title, artist)
        if key != self.track_key:
            self.track_key = key
            self.lyrics = []
            threading.Thread(target=self.fetch_lyrics, args=(title, artist, album, dur), daemon=True).start()
            if d.get("artworkData"):
                self.send_art(d["artworkData"])

        LINK.send("M", title, artist, album, "1" if playing else "0", str(int(pos)), str(int(dur)))
        self.send_lyrics(pos + 0.3)

    def send_art(self, b64):
        try:
            from PIL import Image

            im = Image.open(io.BytesIO(base64.b64decode(b64))).convert("RGB").resize((96, 96), Image.LANCZOS)
            raw = bytearray()
            for r, g, b in im.getdata():
                v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                raw += bytes((v & 0xFF, v >> 8))
            self.art = base64.b64encode(bytes(raw)).decode()
            LINK.send("A", self.art, force=True)
        except Exception as e:
            print("[art]", e)

    def send_lyrics(self, pos):
        L = self.lyrics
        if not L:
            LINK.send("Y", "", "", "纯音乐 / 暂无歌词" if self.lyric_for == self.track_key else "歌词加载中…", "", "")
            return
        i = 0
        for k, (t, _) in enumerate(L):
            if t <= pos:
                i = k
            else:
                break
        get = lambda k: L[k][1] if 0 <= k < len(L) else ""
        LINK.send("Y", get(i - 2), get(i - 1), get(i), get(i + 1), get(i + 2))

    @staticmethod
    def parse_lrc(text):
        out = []
        for line in (text or "").splitlines():
            tags = re.findall(r"\[(\d+):(\d+(?:\.\d+)?)\]", line)
            body = re.sub(r"\[[^\]]*\]", "", line).strip()
            if not tags or not body:
                continue
            for m, s in tags:
                out.append((int(m) * 60 + float(s), body))
        out.sort()
        # drop credit lines like "作词 : xxx"
        return [x for x in out if not re.match(r"^(作词|作曲|编曲|制作|混音|母带|和声|吉他|贝斯|鼓|监制|录音|出品|OP|SP)\s*[:：]", x[1])]

    def fetch_lyrics(self, title, artist, album, dur):
        key = (title, artist)
        lyr = []
        hdr = {"User-Agent": "Mozilla/5.0", "Referer": "https://music.163.com"}
        try:
            r = requests.get("https://music.163.com/api/search/get",
                             params={"s": f"{title} {artist}", "type": 1, "limit": 10}, headers=hdr, timeout=8).json()
            songs = r.get("result", {}).get("songs", []) or []

            def score(sg):
                sc = 0
                if sg.get("name", "").lower() == title.lower():
                    sc += 10
                if any(a.get("name", "") and a["name"] in artist for a in sg.get("artists", [])):
                    sc += 5
                if dur:
                    sc -= abs(sg.get("duration", 0) / 1000 - dur) / 5
                return sc

            if songs:
                best = max(songs, key=score)
                lr = requests.get("https://music.163.com/api/song/lyric",
                                  params={"id": best["id"], "lv": 1}, headers=hdr, timeout=8).json()
                lyr = self.parse_lrc(lr.get("lrc", {}).get("lyric", ""))
        except Exception as e:
            print("[lyrics netease]", e)
        if not lyr:
            try:
                r = requests.get("https://lrclib.net/api/get", params={
                    "track_name": title, "artist_name": artist, "album_name": album, "duration": int(dur)}, timeout=8)
                if r.ok:
                    lyr = self.parse_lrc(r.json().get("syncedLyrics") or "")
            except Exception as e:
                print("[lyrics lrclib]", e)
        if self.track_key == key:
            self.lyrics = lyr
            self.lyric_for = key
            print(f"[lyrics] {title} - {artist}: {len(lyr)} lines")


# ---------------------------------------------------------------- weather
WEEK = "一二三四五六日"
WEATHER = []


def weather_loop():
    city = CFG.get("CITY", "")
    while True:
        try:
            d = requests.get(f"https://wttr.in/{city}", params={"format": "j1", "lang": "zh-cn"}, timeout=15).json()
            cur = d["current_condition"][0]
            desc = (cur.get("lang_zh-cn") or [{}])[0].get("value") or cur["weatherDesc"][0]["value"]
            area = d.get("nearest_area", [{}])[0]
            name = CFG.get("CITY_NAME") or city or area.get("areaName", [{}])[0].get("value", "")
            days = d.get("weather", [])
            today = days[0] if days else {}
            fc = []
            for w in days[:3]:
                dt = datetime.strptime(w["date"], "%Y-%m-%d")
                hd = w["hourly"][4] if len(w.get("hourly", [])) > 4 else w["hourly"][0]
                dd = (hd.get("lang_zh-cn") or [{}])[0].get("value") or hd["weatherDesc"][0]["value"]
                fc.append(f"周{WEEK[dt.weekday()]} {dd} {w['mintempC']}~{w['maxtempC']}°")
            while len(fc) < 3:
                fc.append("")
            WEATHER[:] = ["W", f"{cur['temp_C']}°", desc, name, f"{cur['FeelsLikeC']}°", f"{cur['humidity']}%",
                      f"风速 {cur['windspeedKmph']} km/h", f"{today.get('mintempC', '')}~{today.get('maxtempC', '')}°", *fc]
            time.sleep(15 * 60)
        except Exception as e:
            print("[weather]", e)
            time.sleep(60)


# ---------------------------------------------------------------- keyboard
class Keys:
    NAMES = {"space": "Space", "enter": "Enter", "backspace": "Del", "tab": "Tab", "esc": "Esc",
             "shift": "Shift", "shift_r": "Shift", "ctrl": "Ctrl", "ctrl_r": "Ctrl", "alt": "Opt", "alt_r": "Opt",
             "cmd": "Cmd", "cmd_r": "Cmd", "caps_lock": "Caps", "up": "↑", "down": "↓", "left": "←", "right": "→",
             "delete": "Del", "home": "Home", "end": "End", "page_up": "PgUp", "page_down": "PgDn"}

    def __init__(self):
        self.last = "-"
        self.stream = ""
        self.count = 0
        self.day = datetime.now().date()
        self.times = deque()
        self.dirty = True

    def on_press(self, key):
        try:
            ch = key.char
        except AttributeError:
            ch = None
        if ch and ch.isprintable():
            name = ch.upper() if len(ch) == 1 else ch
            self.stream += ch
        else:
            n = getattr(key, "name", "") or ""
            if n.startswith("f") and n[1:].isdigit():
                name = n.upper()
            else:
                name = self.NAMES.get(n, n.capitalize() or "?")
            if n == "space":
                self.stream += " "
            elif n == "enter":
                self.stream += " · "
            elif n in ("backspace", "delete"):
                self.stream = self.stream[:-1]
        self.stream = self.stream[-28:]
        self.last = name
        if datetime.now().date() != self.day:
            self.day, self.count = datetime.now().date(), 0
        self.count += 1
        self.times.append(time.time())
        self.dirty = True

    def kpm(self):
        now = time.time()
        while self.times and now - self.times[0] > 60:
            self.times.popleft()
        return len(self.times)

    def start(self):
        try:
            from pynput import keyboard

            keyboard.Listener(on_press=self.on_press).start()
        except Exception as e:
            print("[keys] listener failed:", e)

    def push(self):
        LINK.send("K", self.last, self.stream, str(self.count), str(self.kpm()))


# ---------------------------------------------------------------- robot
SYSTEM_PROMPT = CFG.get("BOT_PROMPT") or (
    "你是「小耄」，一个住在桌面小屏幕里的猫猫 AI 机器人，原型是网络上爱哈气的圆头橘猫耄耋。"
    "用口语化的中文回答，简短有趣，一般不超过 60 个字，不要用 markdown。"
    "每次回复最前面加一个情绪标签，只能是 [开心] [难过] [生气] [惊讶] [平静] [调皮] 之一，然后再写回复。")

EMOTIONS = {"开心": "happy", "难过": "sad", "生气": "angry", "惊讶": "surprised", "平静": "calm", "调皮": "playful"}


def split_emotion(reply):
    m = re.match(r"\s*[\[【]([^\]】]{1,4})[\]】]\s*", reply)
    if not m:
        return "calm", reply
    return EMOTIONS.get(m.group(1), "calm"), reply[m.end():]


BOT_MOOD = ["calm"]


def bot_set(state, user="", reply=""):
    LINK.send("R", state, user, reply, BOT_MOOD[0], force=True)


def ask_llm(history):
    provider = CFG.get("LLM_PROVIDER", "openai").lower()
    key = CFG.get("LLM_API_KEY", "")
    model = CFG.get("LLM_MODEL", "")
    if not key:
        return "还没配置 API，请在 ~/.maodie.env 里填 LLM_API_KEY。"
    if provider == "anthropic":
        base = CFG.get("LLM_BASE_URL", "https://api.anthropic.com").rstrip("/")
        r = requests.post(f"{base}/v1/messages", timeout=60, headers={
            "x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json"},
            json={"model": model or "claude-haiku-5-5", "max_tokens": 300, "system": SYSTEM_PROMPT, "messages": history})
        r.raise_for_status()
        return "".join(b.get("text", "") for b in r.json().get("content", []))
    base = CFG.get("LLM_BASE_URL", "https://api.openai.com/v1").rstrip("/")
    r = requests.post(f"{base}/chat/completions", timeout=60, headers={"Authorization": f"Bearer {key}"},
                      json={"model": model, "messages": [{"role": "system", "content": SYSTEM_PROMPT}] + history,
                            "max_tokens": 300})
    r.raise_for_status()
    return r.json()["choices"][0]["message"]["content"]


def download(url, dest_dir):
    name = url.rsplit("/", 1)[1]
    path = os.path.join(dest_dir, name)
    if not os.path.exists(path):
        print(f"[bot] downloading {name} …")
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
    """Return index (in chars) right after a fuzzy "你好耄耋", or -1."""
    from pypinyin import lazy_pinyin

    chars = [c for c in text]
    py = lazy_pinyin(chars)
    for i in range(len(py) - 3):
        if all(py[i + k] in WAKE_SYL[k] for k in range(4)):
            return i + 4
    # also accept just "耄耋" / "mao die" at the start of an utterance
    for i in range(min(3, len(py) - 1)):
        if py[i] in WAKE_SYL[2] and py[i + 1] in WAKE_SYL[3]:
            return i + 2
    return -1


def robot_loop():
    try:
        import numpy as np
        import sherpa_onnx
        import sounddevice as sd
    except Exception as e:
        print("[bot] disabled:", e)
        return
    try:
        vad_path, asr_dir = ensure_models()
    except Exception as e:
        print("[bot] model download failed:", e)
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
    awake_until = 0.0
    busy = threading.Event()
    speaking = [None]  # running `say` process, so a wake word can interrupt it
    last = {"q": "", "r": ""}
    voice = CFG.get("TTS_VOICE", "Tingting")
    dev = CFG.get("MIC_DEVICE") or None
    print(f"[bot] listening on mic: {dev or 'default'}")
    bot_set("idle")

    def answer(question):
        nonlocal history
        busy.set()
        try:
            bot_set("think", question, "")
            history = (history + [{"role": "user", "content": question}])[-10:]
            try:
                mood, reply = split_emotion(ask_llm(history).strip())
            except Exception as e:
                mood, reply = "sad", f"API 出错了：{e}"[:200]
            BOT_MOOD[0] = mood
            history.append({"role": "assistant", "content": reply})
            last["q"], last["r"] = question, reply
            bot_set("speak", question, reply)
            speaking[0] = subprocess.Popen(["say", "-v", voice, reply])
            speaking[0].wait()
            interrupted = speaking[0].returncode not in (0, None)
            speaking[0] = None
            if not interrupted:
                # like xiaozhi: keep listening a few seconds for a follow-up, no wake word needed
                wake_for(8)
                bot_set("listen", question, reply)
        finally:
            busy.clear()

    def wake_for(sec):
        nonlocal awake_until
        awake_until = time.time() + sec

    frames = 0
    with sd.InputStream(samplerate=16000, channels=1, dtype="float32", blocksize=512, device=dev) as mic:
        while True:
            data, _ = mic.read(512)
            frames += 1
            if frames % 3 == 0:  # ~10 Hz voice level for the robot's wave line
                rms = float(np.sqrt(np.mean(data[:, 0] ** 2)))
                LINK.send("V", str(min(100, int(rms * 600))))
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
                print(f"[bot] heard: {text}  wake={w}")
                if busy.is_set():
                    # while thinking/talking only the wake word counts: it interrupts (barge-in)
                    if w >= 0 and speaking[0]:
                        speaking[0].terminate()
                        awake_until = now + 8
                        bot_set("listen", last["q"], last["r"])
                    continue
                if w >= 0:
                    rest = text[w:].strip(" ，,。.!！?？")
                    if len(rest) >= 2:
                        threading.Thread(target=answer, args=(rest,), daemon=True).start()
                    else:
                        awake_until = now + 8
                        bot_set("listen")
                elif now < awake_until:
                    awake_until = 0
                    threading.Thread(target=answer, args=(text,), daemon=True).start()
            if awake_until and time.time() > awake_until and not busy.is_set():
                awake_until = 0
                bot_set("idle", last["q"], last["r"])


# ---------------------------------------------------------------- main
def single_instance():
    import fcntl

    f = open(os.path.join(CACHE, "dashboard.lock"), "w")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print("[main] 另一个 dashboard.py 已经在运行（开机自启的那个），这个退出。")
        time.sleep(30)
        sys.exit(0)
    return f


def main():
    _lock = single_instance()
    print(f"[main] serial {PORT}")
    music = Music()
    threading.Thread(target=weather_loop, daemon=True).start()
    threading.Thread(target=robot_loop, daemon=True).start()
    t_clock = t_art = 0
    while True:
        now = time.time()
        if now - t_clock > 10:
            t_clock = now
            # periodic full resend so a freshly reset board catches up
            LINK.last = {k: v for k, v in LINK.last.items() if k == "A"}
            LINK.send("C", str(int(now)), str(int(datetime.now().astimezone().utcoffset().total_seconds())), force=True)
        music.poll()
        if WEATHER:
            LINK.send(*WEATHER)
        if music.art and now - t_art > 30:
            t_art = now
            LINK.send("A", music.art, force=True)
        time.sleep(0.4)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
