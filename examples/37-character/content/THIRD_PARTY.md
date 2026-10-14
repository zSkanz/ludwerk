# Whose work this content is

The models and the clips of this example are cut from three packs their
authors gave to the public domain. Each is **CC0 1.0** -- no rights reserved,
no credit owed -- and the credit is here anyway. The packs' own licence files
are in `licences/`, as they came.

Nothing is fetched to build or to run the example: `tools/make_content.py`
was run once against the packs as downloaded, and what it wrote is these
files.

## KayKit: Adventurers Character Pack, version 1.0

- By Kay Lousberg, <https://www.kaylousberg.com>. CC0 1.0
  (`licences/KayKit-Adventurers-1.0-LICENSE.txt`).
- Had from <https://github.com/KayKit-Game-Assets/KayKit-Character-Pack-Adventures-1.0>
  at commit `672074b73ba276876a19e8816ecdc5241817ab47`.
- From it:
  - `clips/adventurer.glb` -- the rig of the pack's rogue, one of its meshes,
    and eleven of its seventy-six clips under the names the graph uses.
    Changed here: made four fifths the size; a joint added, `String`, under
    the right hand's slot, with a track in two clips; a clip added, `AimUp`.
  - `models/rogue.glb` -- the hooded rogue without its head and without what
    was in its hands, four fifths the size, no clips.
  - `models/rogue_hood.glb`, `models/rogue_head.glb` -- the hooded rogue's
    head and the rogue's, each alone on the skeleton, four fifths the size.

## KayKit: Character Pack: Skeletons, version 1.0

- By Kay Lousberg, <https://www.kaylousberg.com>. CC0 1.0
  (`licences/KayKit-Skeletons-1.0-LICENSE.txt`).
- Had from <https://github.com/KayKit-Game-Assets/KayKit-Character-Pack-Skeletons-1.0>
  at commit `15b62b9bad122f72926c10fb14d622c73819fa54`.
- From it: `models/skeleton.glb` -- the skeleton minion, made 0.78 as wide and
  deep and 0.92 as tall, no clips.

## Universal Animation Library, standard edition

- By Quaternius, <https://quaternius.com>. CC0 1.0
  (`licences/Universal-Animation-Library-LICENSE.txt`).
- Had from a mirror of the pack's glTF,
  <https://github.com/J-Ponzo/gltf-universal-animation-library> at commit
  `e24c23cf2a1323488a3faa226ea7ea21f644b73e`.
- From it: `models/mannequin.glb` -- the pack's mannequin as it comes, without
  its clips.

## Made here

- `models/crossbow.glb` is drawn by `tools/make_content.py` and is this
  repository's own, under its licence.
