"""One line in three languages of three lengths, for the determinism scenario:
half a second in the default language, nine tenths in Brazilian Portuguese and
three tenths in Japanese. The scenario is replayed heard in each, and its
trace must not move.

    python tools/make_voices.py        (from tests/determinism/dubbed)
"""

import math
import os
import struct
import wave

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def tone(relative: str, seconds: float, hertz: float, rate: int) -> None:
    path = os.path.join(HERE, "content", *relative.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    frames = round(seconds * rate)
    with wave.open(path, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        data = bytearray()
        for frame in range(frames):
            sample = int(12000 * math.sin(2 * math.pi * hertz * frame / rate))
            data += struct.pack("<h", sample)
        out.writeframes(bytes(data))
    print(f"{relative}: {seconds} s at {rate} Hz")


tone("voice/line.wav", 0.5, 440.0, 48000)
tone("l10n/pt-BR/voice/line.wav", 0.9, 330.0, 24000)
tone("l10n/ja/voice/line.wav", 0.3, 550.0, 44100)
