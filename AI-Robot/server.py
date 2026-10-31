import io
import os
os.environ["HF_HUB_DISABLE_SYMLINKS_WARNING"] = "1"
import tempfile
import wave
import threading
import traceback
import shutil
import subprocess
import urllib.parse

import requests
import pyttsx3
from dotenv import load_dotenv
from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import Response

# Fix compatibility between PyAV and faster-whisper (metadata_errors was removed in newer PyAV versions)
import av
_orig_av_open = av.open
def _safe_av_open(*args, **kwargs):
    kwargs.pop("metadata_errors", None)
    return _orig_av_open(*args, **kwargs)
av.open = _safe_av_open

# Fix pyttsx3 espeak voice bug on Linux (where default voice was mistakenly set to 'gmw/en')
try:
    import pyttsx3.drivers.espeak as espeak_driver
    espeak_driver.EspeakDriver._defaultVoice = "en"
except Exception:
    pass

try:
    import audioop
except ImportError:
    raise RuntimeError(
        "Missing 'audioop' module. On Python 3.13+, run: pip install audioop-lts"
    )

from faster_whisper import WhisperModel

app = FastAPI(title="ESP32 AI Robot")

load_dotenv()
OLLAMA_API_KEY = os.getenv("OLLAMA_API_KEY")
WHISPER_LANGUAGE = os.getenv("WHISPER_LANGUAGE", "en")  # Default to English (prevents foreign hallucinations on static)

OLLAMA_URL = "https://ollama.com/api/chat"
MODEL = "beam:cloud"

# Load the speech-recognition model once.
print("Loading Whisper model...")
whisper = WhisperModel(
    "base",
    device="cpu",
    compute_type="int8"
)
print("Whisper model loaded successfully!")

tts_lock = threading.Lock()


def safe_header(text, max_len=200):
    """Ensure HTTP headers only contain latin-1/ASCII characters (RFC standard)."""
    if not text:
        return ""
    clean = text.replace("\r", " ").replace("\n", " ").strip()[:max_len]
    try:
        clean.encode("latin-1")
        return clean
    except UnicodeEncodeError:
        # Fallback to URL-encoded ASCII for any non-latin-1 Unicode text
        return urllib.parse.quote(clean)


@app.get("/")
def home():
    return {
        "status": "Robot AI server is running!",
        "voice_endpoint": "/voice",
        "chat_endpoint": "/chat"
    }


@app.post("/chat")
async def chat(request: Request):
    data = await request.json()
    message = data.get("message", "")
    if not message:
        raise HTTPException(400, "Missing 'message' in request body")
    reply = ask_ai(message)
    return {"reply": reply}


def make_wav(pcm_bytes, sample_rate=16000):
    """Convert incoming mono 16-bit PCM into a WAV file."""
    audio = io.BytesIO()

    with wave.open(audio, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm_bytes)

    return audio.getvalue()


def ask_ai(message):
    headers = {}
    if OLLAMA_API_KEY:
        headers["Authorization"] = f"Bearer {OLLAMA_API_KEY}"

    response = requests.post(
        OLLAMA_URL,
        headers=headers,
        json={
            "model": MODEL,
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "You are a friendly educational robot. "
                        "Give short, natural answers suitable for speaking. "
                        "Reply in the same language as the user."
                    ),
                },
                {
                    "role": "user",
                    "content": message,
                },
            ],
            "stream": False,
        },
        timeout=180,
    )
    response.raise_for_status()
    data = response.json()
    return data["message"]["content"].strip()


def make_speech_wav(text):
    """Create speech and convert it to 16 kHz mono PCM WAV."""
    with tempfile.TemporaryDirectory() as folder:
        original = os.path.join(folder, "speech_original.wav")
        generated = False

        # On Linux / EC2: prefer direct espeak-ng / espeak CLI (avoids pyttsx3 threading/voice bugs)
        tts_cmd = shutil.which("espeak-ng") or shutil.which("espeak")
        if tts_cmd:
            try:
                subprocess.run(
                    [tts_cmd, "-v", "en", "-w", original, text],
                    check=True,
                    timeout=20
                )
                if os.path.exists(original) and os.path.getsize(original) > 44:
                    generated = True
            except Exception as e:
                print("espeak CLI warning:", e)

        # Fallback to pyttsx3 (for Windows or if CLI is not found)
        if not generated:
            with tts_lock:
                engine = pyttsx3.init()
                engine.save_to_file(text, original)
                engine.runAndWait()
                engine.stop()

        # Read speech WAV and convert to signed 16-bit mono 16 kHz PCM
        with wave.open(original, "rb") as source:
            channels = source.getnchannels()
            width = source.getsampwidth()
            rate = source.getframerate()
            frames = source.readframes(source.getnframes())

        if width != 2:
            frames = audioop.lin2lin(frames, width, 2)

        if channels == 2:
            frames = audioop.tomono(frames, 2, 0.5, 0.5)

        if rate != 16000:
            frames, _ = audioop.ratecv(
                frames, 2, 1, rate, 16000, None
            )

        return make_wav(frames, 16000)


@app.post("/voice")
async def voice(request: Request):
    # ESP32 sends raw, mono, 16-bit PCM at 16 kHz.
    pcm = await request.body()
    print(f"\n--> [Step 1] Received {len(pcm)} bytes of PCM audio from ESP32")

    if len(pcm) < 4000:
        raise HTTPException(400, "Audio is too short (< 0.25s)")

    try:
        with tempfile.NamedTemporaryFile(
            suffix=".wav", delete=False
        ) as temp:
            temp.write(make_wav(pcm))
            input_path = temp.name

        try:
            print("--> [Step 2] Transcribing with Whisper...")
            segments, info = whisper.transcribe(
                input_path,
                language=WHISPER_LANGUAGE if WHISPER_LANGUAGE else None,
                beam_size=1
            )
            transcript = " ".join(
                segment.text.strip() for segment in segments
            ).strip()
            print(f"    Transcript: '{transcript}'")
        finally:
            if os.path.exists(input_path):
                os.unlink(input_path)

        if not transcript:
            transcript = "Please ask me a question."

        print("--> [Step 3] Asking AI (Ollama)...")
        answer = ask_ai(transcript)
        print(f"    AI Answer: '{answer}'")

        print("--> [Step 4] Synthesizing speech...")
        speech = make_speech_wav(answer)
        print(f"    Speech WAV ready: {len(speech)} bytes")

        return Response(
            content=speech,
            media_type="audio/wav",
            headers={
                "X-Transcript": safe_header(transcript),
                "X-Robot-Reply": safe_header(answer)
            }
        )

    except Exception as error:
        print("\n!!! VOICE PROCESSING ERROR !!!")
        traceback.print_exc()
        raise HTTPException(500, detail=str(error))


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
