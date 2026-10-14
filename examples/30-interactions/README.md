# 30-interactions

A room where a click is the engine's and the rest is the game's own code. The
red button on the wall has a `ClickDetector`: click it -- or tap it -- and the
lamp turns on. The door opens when you hold E near it: the server measures
the distance from your character and how long you held, and the client shows a
hint while you are in reach. The drawer in the desk and the lever on the west
wall are taken hold of with a click and moved with the pointer: the client
sends the pointer's ray while it holds one, and the server moves it, along the
runners or about the hinge, within its limits.

Run it with `run.bat`. WASD walks.
