# 30-interactions

A room used without a line of input code (ADR 0126). A red button on the wall
has a `ClickDetector`: click it -- or tap it -- and the lamp turns on. The door
has a `ProximityPrompt`: walk up to it and a prompt appears; hold E, or the
gamepad's X, or tap the prompt, and the door swings open.

The engine casts the pointer into the world once a tick, measures the reach
from your character, draws the prompt with its hold ring, and fires the events
on the authority with the player who pressed -- so the server's script is the
whole of the room's behaviour, and the same in a match as alone.

Run it with `run.bat`. WASD walks.
