# Content and asset URNs

Every file a game loads is named by a **content URN**, and there is one scheme:

```text
asset://models/tree.glb
asset://textures/bark.png
asset://audio/wind.ogg
asset://fonts/Inter.ttf
```

`asset://` plus the file's path **relative to the project's content directory**,
with forward slashes on every platform.

## Where they are used

The properties that take one are typed `Content`, which is a string today and is
reserved to become opaque later:

| Member | Names |
|---|---|
| `MeshPart.MeshContent` | A mesh file. |
| `ImageLabel.Image` | A picture. |
| `TextLabel.Font` | A TrueType face. |
| `Sound.Content` | An audio file. |
| `InputBinding.Image` | A prompt glyph. |
| `AnimationPlayer.LoadAnimation` | A clip. |
| `AudioService.PlayLocal` | An audio file. |

## What resolves a URN

Content is served by **mounts**, and a URN resolves through them in reverse
order — **a later mount wins**. For a project, the order is:

1. The loose `content/` directory in the project.
2. The compiled pack, if one has been built.
3. The built chunk directory.

So a loose file overrides a compiled one, which is what makes development
iteration work: drop a PNG in `content/textures/` and it is there, with no build
step, and the same URN will pick up the compiled version once a build exists.

Path traversal — `asset://../../somewhere` — is refused at this layer rather
than at the filesystem, because a shipped game mounting a pack has no filesystem
to refuse it.

## A missing file is visible, not silent

Each kind of content fails in the way that is loudest without crashing:

- **A mesh** that fails to import leaves the previous geometry in place and says
  why. A mesh whose points have not arrived collides as its bounding box.
- **An image** that names nothing draws as a flat rectangle of `ImageColor` —
  the same as one still loading, because from a frame's point of view they are.
- **A font** that cannot be resolved falls back to the default face and warns
  **once**, not once a frame.
- **A sound** that names nothing plays a generated tone whose pitch comes from a
  hash of the id. Deliberately audible: a missing sound should be noticed.

For audio there is a number that answers "is this the real file or the
placeholder", which a person listening on laptop speakers often cannot:

```luau
--!strict
local DebugService = game:GetService("DebugService")
print(DebugService:GetStat("AudioClipsLoaded"), DebugService:GetStat("AudioClipsMissing"))
```

## A script cannot open a file

There is no filesystem in the game VM. A script names content through the
properties above and the engine loads it; there is no read, no write, and no
directory listing.

That is why persistence is a backend rather than a save file — see
[Talking to a backend](manual:guides/backend).

## Content-addressed underneath

Inside a compiled pack, a blob is stored and looked up by a hash of its
contents rather than by its name. Two assets with identical bytes are stored
once, and a streamed chunk referring to a shared mesh refers to the hash.

The URN is what you write; the hash is what the engine resolves it to. You do
not need to think about the second one until you are looking at a manifest.

## Loading before it is shown

A mesh or a picture is loaded the first time something in the world names it,
and appears a moment later. When that moment matters -- the first frame of a
level, a cutscene -- load it first with `ContentProvider:PreloadAsync`, behind
a loading screen:

```luau
--!strict
local ContentProvider = game:GetService("ContentProvider")

local level = workspace:WaitForChild("Level")
ContentProvider:PreloadAsync({ "asset://models/boss.gltf", level }, function(item, status)
    print(item, status.Name) -- Success or Failure, once per item
end)
-- Everything named above is loaded now.
```

An item is a URN, or an instance: every asset it and its descendants name.
The call yields until each has arrived or failed, and
`ContentProvider.RequestQueueSize` says how many are still on their way.
Meshes and pictures are loaded onto the GPU; a sound is opened and its length
read. A scene prepared with `SceneService:LoadSceneAsync` loads its meshes the
same way ([Scenes at run time](manual:guides/scenes#loading-in-the-background)).

## Where to look next

- [The asset pipeline](manual:assets/pipeline) — what turns a source file into
  a mounted one
- [Meshes and models](manual:world/meshes)
