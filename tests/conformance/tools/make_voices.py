"""The two recordings `world/dubbing.spec.luau` plays: one line, a fifth of a
second of a 1,000 Hz tone in the default language and two fifths of a second
of a 200 Hz tone "in Portuguese", nearly at full scale -- so a spec can tell
which was found by its length and by which band it lights.

    python tests/conformance/tools/make_voices.py     (from the repository root)
"""

import math
import os
import struct
import wave

CONTENT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "content")


def tone(relative: str, seconds: float, hertz: float, rate: int) -> None:
    path = os.path.join(CONTENT, *relative.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        data = bytearray()
        for frame in range(round(seconds * rate)):
            data += struct.pack("<h", int(32000 * math.sin(2 * math.pi * hertz * frame / rate)))
        out.writeframes(bytes(data))
    print(f"{relative}: {seconds} s at {rate} Hz")


tone("voice/spec/hello.wav", 0.2, 1000.0, 48000)
tone("l10n/pt-BR/voice/spec/hello.wav", 0.4, 200.0, 24000)
