# 31-lake-and-river

A lake and the river that feeds it, over terrain (ADR 0118). The lake is a
`Water` of shape `Box`: water at one level over a rectangle, as deep as its
`Size.Y`, with two low ripples on it. The river is a `Water` of shape `Spline`:
a ribbon `Size.X` wide along the `WaterPoint`s under it, whose water moves from
the first point to the last at `FlowSpeed`.

Every few seconds a log -- a cylinder of density 0.5 and nothing else -- drops
in at the head of the river. The water holds it up and drags it along with the
flow, round the bends, out into the lake, where a breeze (`Current`) is all that
moves it.

The script moves nothing: it carves the ground and says where the water is.

Where the river runs out into the lake the two overlap, and the rule is that
the water whose surface is highest there holds a point, and of two at one level
the one that moves -- so the river carries a log until it is in the lake.

Run it with `run.bat`.
