
import io
import os
import tempfile
import wave
import threading

import requests
import pyttsx3
from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import Response
from faster_whisper import WhisperModel

app = FastAPI(title="ESP32 AI Robot")

OLLAMA_URL = "http://127.0.0.1:11434/api/generate"
MODEL = "llama3.2:1b"

# Load the speech-recognition model once.
# First startup may download the model.
whisper = WhisperModel(
    "base",
    device="cpu",
    compute_type="int8"
)

tts_lock = threading.Lock()


@app.get("/")
def home():
    return {
        "status": "Robot AI server is running!",
        "voice_endpoint": "/voice"
    }


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
    response = requests.post(
        OLLAMA_URL,
        json={
            "model": MODEL,
            "prompt": (
                "You are a friendly educational robot. "
                "Give short, natural answers suitable for speaking. "
                "Reply in the same language as the user. "
                "Support Sinhala and English when possible.\n\n"
                f"User: {message}\nRobot:"
            ),
            "stream": False
        },
        timeout=180
    )
    response.raise_for_status()
    return response.json()["response"].strip()


def make_speech_wav(text):
    """Create speech and convert it to 16 kHz mono PCM WAV."""
    with tts_lock:
        engine = pyttsx3.init()

        with tempfile.TemporaryDirectory() as folder:
            original = os.path.join(folder, "speech_original.wav")
            engine.save_to_file(text, original)
            engine.runAndWait()
            engine.stop()

            # Read speech WAV. Windows voices may use another
            # sample rate, channel count, or sample width.
            with wave.open(original, "rb") as source:
                channels = source.getnchannels()
                width = source.getsampwidth()
                rate = source.getframerate()
                frames = source.readframes(source.getnframes())

            # Convert samples to signed 16-bit mono.
            import audioop

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

    if len(pcm) < 4000:
        raise HTTPException(400, "Audio is too short")

    try:
        with tempfile.NamedTemporaryFile(
            suffix=".wav", delete=False
        ) as temp:
            temp.write(make_wav(pcm))
            input_path = temp.name

        try:
            segments, info = whisper.transcribe(
                input_path,
                beam_size=1
            )
            transcript = " ".join(
                segment.text.strip() for segment in segments
            ).strip()
        finally:
            os.unlink(input_path)

        if not transcript:
            transcript = "Please ask me a question."

        answer = ask_ai(transcript)
        speech = make_speech_wav(answer)

        return Response(
            content=speech,
            media_type="audio/wav",
            headers={
                "X-Transcript": transcript[:200],
                "X-Robot-Reply": answer[:200]
            }
        )

    except Exception as error:
        print("VOICE ERROR:", repr(error))
        raise HTTPException(500, str(error))
