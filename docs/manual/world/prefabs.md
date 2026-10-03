# Prefabs: stamps

A **stamp** is an instance tree saved as a file -- `content/stamps/<name>.stamp.json`
-- and placed many times. A placed stamp is a **copy**, and a copy stays linked
to its file: change the stamp and every copy changes, except where a copy has
its own value. That is what other engines call a prefab, and this page is the
whole model, with one worked example running through it.

## The example: a fighter and its classes

A game has fighters. Every fighter has a body, a hat and a weapon socket; a
Scout is a fighter with a scope, a Heavy is one with armour, and each fighter in
a match has a team colour.

**Make the stamp.** Build the fighter in the scene -- a `Model` named `Fighter`
with its parts -- then right-click it in the Explorer and choose **Convert to
stamp**, and call it `fighter`. That writes `content/stamps/fighter.stamp.json`
and turns the model you built into the first copy of it.

**Place copies.** Drag `fighter` from Content into the scene, or from a script:

```luau
--!strict
local fighter = Instance.stamp("fighter")
fighter:PivotTo(CFrame.new(10, 0, 0))
fighter.Parent = workspace
```

## A copy has its own values: overrides

Change anything on a copy -- its hat's colour, its body's size -- and that one
value is the copy's **override**. Properties marks it with a dot, and its menu
offers to **Revert to stamp** or **Apply to stamp**, which writes it into the
file so every other copy follows. The Explorer's **Stamp** menu on a copy
reverts one part or the whole copy, and **Apply the whole copy to its stamp**
makes the stamp what the copy is now: edit a fighter where it stands in the
level, then push all of it up.

A copy's place is its own, not an override: move it anywhere and its parts stay
where the stamp puts them, relative to it. Raise the hat in the stamp file and
it rises on every fighter, wherever each one stands.

## Children of its own, and children left out

A copy may hold more than its stamp: give one fighter a flag and it stays a
fighter, with a flag. And it may leave a stamp's child out: delete its hat, or
choose **Disable** on the hat in the Explorer. A disabled child is listed,
greyed, under **Stamp → Disabled** on the copy, with **Enable** to build it back
as the stamp has it. A stamp's part cannot be deleted from a copy for good --
the stamp still has it, and every change to the stamp still reaches the rest.

## Variants: the Scout

A **variant** is a stamp that is a copy of another. Place a fighter, give it a
scope and a lighter body, then choose **Stamp → Make variant...** and call it
`scout`. The file holds only what the Scout has of its own; the copy you made it
from becomes the first Scout.

Change the base -- new boots on `fighter` -- and every Scout gets them, except
where the Scout says otherwise. A variant of a variant works the same way: a
Ranger is a Scout with a cloak. When a copy of a Scout has a value worth keeping,
**Apply to** in its menu asks which file it goes to: the Scout, or the fighter
underneath it.

**Open base** in the Stamp menu opens the stamp a variant was made from.

## Stamps inside stamps

A stamp may hold copies of other stamps -- a squad stamp holding three
fighters -- and each stays linked to its own file. A stamp that holds a copy of
itself is refused, and the log names the chain that reached it.

## Parameters: what a designer sets

Most of what makes one fighter different from another is a handful of values.
A stamp **declares** them, and each copy shows them first in Properties.

Open the stamp (**Stamp → Open stamp**), select its root, and under
**Parameters** add one: `TeamColor`, a colour. Then right-click the hat's
`Color` and choose **Drive from parameter → TeamColor**. Save the stamp.

Every copy now has `TeamColor` at the top of Properties, and setting it colours
its hat. A parameter is an **attribute** of the copy's root, so a script reads
and writes it like any attribute, and it replicates like one:

```luau
--!strict
local red = Instance.stamp("fighter", true, { TeamColor = Color3.fromRGB(200, 40, 40) })
red.Parent = workspace

red:SetAttribute("TeamColor", Color3.fromRGB(40, 80, 200)) -- the hat follows at once
print(red:GetStamp()) --> stamps/fighter.stamp.json
```

A parameter may have a range (a number) or a list of choices (a text). A value
outside them is refused with an error naming the parameter -- never clamped.

## Building parts from parameters: `Construct`

Some stamps are made of a number of things a parameter decides: a fence's
posts, a staircase's steps. Put a `ModuleScript` named `Construct` under the
stamp's root, returning a function of the copy, its parameters and a `Random`:

```luau
--!strict
return function(root: Instance, parameters: { [string]: any }, random: Random)
    for at = 1, parameters.Posts :: number do
        local post = Instance.new("Part")
        post.Name = `Post{at}`
        post.Anchored = true
        post.Position = vector.create(at * 2, 0, 0)
        post:SetAttribute("Lean", random:NextNumber())
        post.Parent = root
    end
end
```

It runs when a copy is placed or loaded, and in the editor whenever a
parameter is changed. It does **not** run again in play when a parameter is
written: a construction step builds, and the game moves on. What it builds is
never saved -- it is built again each time -- and the `Random` is seeded from
the stamp and the parameters, so the same values build the same posts on every
machine.

## Spawning many: preload and pools

The first copy of a stamp reads its file. Ask for it ahead:

```luau
--!strict
local ContentProvider = game:GetService("ContentProvider")
ContentProvider:PreloadAsync({ "fighter" })
```

A name with no `://` is a stamp; it is read once and kept, with every asset it
names.

For things spawned and gone again and again, `@engine/stamppool` keeps copies
ready:

```luau
--!strict
local StampPool = require("@engine/stamppool")
local sparks = StampPool.new("spark", 8)

local spark = sparks:Take(workspace)
-- ...
sparks:Return(spark)
```

A copy given back is not handed out again as it is -- whatever the game did to
it would come with it -- it is replaced by a fresh one, a copy a frame.

## Tidying

- **Select every copy** in the Stamp menu selects every copy of that stamp.
- **Replace with stamp...** swaps a copy for a copy of another stamp, where it
  stands, keeping the overrides the new stamp has parts for.
- When a stamp loses a part a copy had an override for, the copy keeps the
  override -- a stamp that gets the part back finds it -- and the Stamp menu
  offers **Clean up unused overrides**.
- **Break stamp** unlinks a copy for good: it becomes an ordinary tree.

## Without a file: `Clone`

A tree made in code and kept unparented is a template too, and `Clone` copies
it -- children, properties, attributes, tags, and references inside it fixed up
to the copy. That is the right tool for something only code makes; a stamp is
for something somebody lays out.

## Where to look next

- [Scenes: the world as data](manual:world/scenes) -- the format a stamp shares
- [Properties, attributes and tags](manual:concepts/properties) -- where a
  parameter lives
- [`Instance:GetStamp`](api:Instance.GetStamp)
