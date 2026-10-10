"""The two recordings the dubbing gate plays: one line, half a second in the
default language and nine tenths in Brazilian Portuguese -- and at another
sample rate, so a length is seen to be measured at the mixer's rate and not
counted in the file's own frames.

    python tools/make_voices.py        (from tests/netcode/dubbed)

Tones, not speech: what the gate holds is which file is found and how long a
line lasts, and a tone is forty lines of arithmetic anybody can read.
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


tone("voice/act1/hello.wav", 0.5, 440.0, 48000)
tone("l10n/pt-BR/voice/act1/hello.wav", 0.9, 330.0, 24000)
