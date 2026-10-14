"""Makes the recordings of `content/dialogue/booth.lines.json`, in English and
in Brazilian Portuguese, and the viseme track beside each.

Run it from this folder (`python tools/make_voices.py`) after changing a
line's words; the files it writes are checked in, so nothing needs Python to
run the example.

**These are not voices.** They are a few lines of formant synthesis -- a
buzz at a speaker's pitch through three resonances that move from vowel to
vowel -- reading each line's own letters, so that the example has something
to play in two languages without anybody's recording in the repository. They
are in the place a studio's files go, under the names a studio's files have:
replace them and nothing else changes.

What it shows of the layout (ADR 0200):

- `content/voice/booth/<line>.wav` is the line in the project's default
  language, and `content/l10n/pt-BR/voice/booth/<line>.wav` the same line in
  Portuguese: the same path, under `l10n/<locale>/`.
- `<line>.visemes.json` beside a recording is its mouth: which shape, from
  when to when. The free lip-sync tools write this file from a recording
  (`ludwerk voice --visemes` runs one); here the synthesizer knows what it
  said and writes it itself.
- `bruno_2` is recorded in English alone, and `clara_2` not at all: what a
  game does about a line nobody dubbed, and one nobody recorded.
"""

import json
import math
import random
import struct
import unicodedata
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RATE = 16000

# A vowel: its three resonances in hertz, and the mouth shape that says it.
VOWELS = {
    "a": ((730, 1090, 2440), "D"),
    "e": ((530, 1840, 2480), "C"),
    "i": ((270, 2290, 3010), "B"),
    "o": ((570, 840, 2410), "E"),
    "u": ((300, 870, 2240), "F"),
}
# A consonant: how it sounds -- shut, a hum, a hiss -- and its mouth shape.
SHUT, HUM, HISS = "shut", "hum", "hiss"
CONSONANTS = {
    **{letter: (SHUT, "A") for letter in "bp"},
    "m": (HUM, "A"),
    **{letter: (HISS, "G") for letter in "fv"},
    "l": (HUM, "H"),
    **{letter: (HUM, "B") for letter in "nr"},
    **{letter: (HISS, "B") for letter in "csxzjh"},
    **{letter: (SHUT, "B") for letter in "dgkqt"},
}
VOWEL_SECONDS, CONSONANT_SECONDS, SPACE_SECONDS, STOP_SECONDS = 0.105, 0.055, 0.045, 0.16

# Who speaks at what pitch, in hertz.
PITCH = {"anna": 205.0, "bruno": 112.0, "clara": 240.0}
# Which language has a recording of which line. The default language first.
RECORDED = {
    "en": ("anna_1", "bruno_1", "clara_1", "anna_2", "bruno_2"),
    "pt-BR": ("anna_1", "bruno_1", "clara_1", "anna_2"),
}


def letters(text):
    """`text` as the plain letters the two tables know: accents off, `y` and
    `w` said as the vowels they are nearest."""
    plain = unicodedata.normalize("NFD", text.lower())
    plain = "".join(c for c in plain if unicodedata.category(c) != "Mn")
    return plain.replace("y", "i").replace("w", "u")


def segments(text):
    """What is said, a piece at a time: (seconds, kind, resonances, shape)."""
    said = []
    for letter in letters(text):
        if letter in VOWELS:
            resonances, shape = VOWELS[letter]
            said.append((VOWEL_SECONDS, "vowel", resonances, shape))
        elif letter in CONSONANTS:
            kind, shape = CONSONANTS[letter]
            said.append((CONSONANT_SECONDS, kind, None, shape))
        elif letter == " ":
            said.append((SPACE_SECONDS, "rest", None, "X"))
        elif letter in ",.;:!?":
            said.append((STOP_SECONDS, "rest", None, "X"))
    # A line ends when its last sound does, not after a full stop's silence.
    while said and said[-1][1] == "rest":
        said.pop()
    return said


class Resonance:
    """A two-pole resonator: what a throat does to a buzz at one frequency."""

    def __init__(self, width):
        self.r = math.exp(-math.pi * width / RATE)
        self.one = self.two = 0.0

    def step(self, sample, hertz):
        out = (1.0 - self.r) * sample + 2.0 * self.r * math.cos(2.0 * math.pi * hertz / RATE) * self.one
        out -= self.r * self.r * self.two
        self.two, self.one = self.one, out
        return out


def speak(text, pitch, seed):
    """The samples of `text` said at `pitch`, and its cues."""
    said = segments(text)
    total = sum(piece[0] for piece in said)
    noise = random.Random(seed)
    throat = [Resonance(90.0), Resonance(110.0), Resonance(170.0)]
    shares = (1.0, 0.55, 0.30)
    at = [500.0, 1500.0, 2500.0]
    samples, cues = [], []
    phase, level, clock = 0.0, 0.0, 0.0
    for seconds, kind, resonances, shape in said:
        if cues and cues[-1]["value"] == shape:
            cues[-1]["end"] = round(clock + seconds, 3)
        else:
            cues.append({"start": round(clock, 3), "end": round(clock + seconds, 3), "value": shape})
        for _ in range(round(seconds * RATE)):
            through = clock / total
            # A voice falls across a sentence, and is never quite steady.
            hertz = pitch * (1.09 - 0.17 * through) * (1.0 + 0.025 * math.sin(2.0 * math.pi * 5.3 * clock))
            phase = (phase + hertz / RATE) % 1.0
            buzz = 2.0 * phase - 1.0
            if kind == "vowel":
                wanted, source = 1.0, buzz
            elif kind == HUM:
                wanted, source = 0.28, buzz
            elif kind == HISS:
                wanted, source = 0.20, noise.uniform(-1.0, 1.0)
            else:
                wanted, source = 0.0, buzz
            # Ten milliseconds from one loudness to the next: no click.
            level += (wanted - level) * (1.0 / (0.010 * RATE))
            if resonances is not None:
                for index in range(3):
                    at[index] += (resonances[index] - at[index]) * (1.0 / (0.025 * RATE))
            sample = sum(shares[i] * throat[i].step(source, at[i]) for i in range(3))
            samples.append(sample * level)
            clock += 1.0 / RATE
    peak = max(abs(sample) for sample in samples) or 1.0
    return [sample * 0.72 / peak for sample in samples], cues


def main():
    lines = json.loads((ROOT / "content" / "dialogue" / "booth.lines.json").read_text(encoding="utf-8"))["lines"]
    for locale, recorded in RECORDED.items():
        words = json.loads((ROOT / "i18n" / f"{locale}.json").read_text(encoding="utf-8"))
        folder = ROOT / "content" / "voice" / "booth"
        if locale != "en":
            folder = ROOT / "content" / "l10n" / locale / "voice" / "booth"
        folder.mkdir(parents=True, exist_ok=True)
        for seed, line in enumerate(recorded):
            text = words[f"booth.{line}"]
            samples, cues = speak(text, PITCH[lines[line]["speaker"]], seed + 1)
            with wave.open(str(folder / f"{line}.wav"), "wb") as out:
                out.setnchannels(1)
                out.setsampwidth(2)
                out.setframerate(RATE)
                out.writeframes(b"".join(struct.pack("<h", int(sample * 32767)) for sample in samples))
            (folder / f"{line}.visemes.json").write_text(
                json.dumps({"cues": cues}, indent=1) + "\n", encoding="utf-8", newline="\n"
            )
            print(f"{locale} {line}: {len(samples) / RATE:.2f} s, {len(cues)} cue(s)")


main()
