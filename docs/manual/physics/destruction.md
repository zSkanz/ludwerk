# Destruction

A wall that breaks is a part cut into pieces, held together by joints that
give when something hits them hard enough.

```luau
local wall = workspace.Wall                   -- a Part, Shape = Block
local pieces = wall:Fracture({ Pieces = 40, Seed = 7 })
```

`Fracture` returns a `Model` of pieces where the part was, and the part is
gone. Each piece is a copy of the part -- its material, its color, how it
collides -- and every two that share a face are held by a `FixedConstraint`
that breaks under `Strength` newtons. Until something strikes them, they
stand as the part stood.

| option | what it is |
|---|---|
| `Pieces` | how many, 1 to 64; 8 if not given |
| `Seed` | which cut: the same seed and the same part give the same pieces, on every machine |
| `Strength` | newtons a joint holds; by default twice the part's weight |

The largest piece is cut across its longest side, again and again, so the
pieces are of different sizes, as broken things are, and none is a sliver.
Only a `Part` whose `Shape` is `Block` can be cut. A model already made of
parts -- cut ahead in a modeling tool -- is held together the same way, with
the same joints.

## A wall keeps its footing

A part that was anchored keeps its footing: an anchored `Footing` part under
its bottom, which does not collide, holds the pieces along the bottom by
joints four times as strong. A blow then breaks the wall where it lands and
the rest stands; a much harder one tears it from its footing too.

## Dust and chips are particles

A joint that breaks fires `Broken`. That is where dust and chips belong:

```luau
for _, joint in pieces:GetDescendants() do
    if joint:IsA("FixedConstraint") then
        joint.Broken:Connect(function()
            dust.Position = joint.Attachment0.WorldCFrame.Position
            puff:Emit(12)             -- a ParticleEmitter under `dust`
        end)
    end
end
```

**Pieces are simulation; particles are only seen.** What must block a
corridor, be stood on or be pushed is a piece. What is only seen -- dust,
chips, sparks -- is a particle burst: drawn on each machine, simulated
nowhere, gone by its own lifetime.

## In a match

The pieces are parts like any other: the server's, and sent to every player as
they move. Measured, an eight-meter wall cut into fifty pieces costs each
player nothing while it stands -- the pieces held by their joints sleep --
about 106 KB a second in the three seconds after it is struck, and 39 KB in
the three after that. Cut walls into as few pieces as the moment needs, and
make the rest particles. A particle burst is sent as the number emitted and
nothing more.

## What it does not do

- Cut anything but a box.
- Weaken a joint by blows: a joint breaks at once over its strength, and
  holds fully under it.
- Remove pieces by themselves: remove what you no longer need, as any part.

## Where to look next

- [Welds and constraints](manual:physics/joints) -- `BreakForce`, `BreakTorque` and `Broken`
- [Vehicles](manual:physics/vehicles)
- [`BasePart:Fracture`](api:BasePart)
