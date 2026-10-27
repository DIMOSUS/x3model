# x3model — X3: Terran Conflict / Albion Prelude models and scenes ⇄ FBX

Converts the game's bodies (`.bob/.pbb`, `.bod/.pbd`) and scenes (`CUT1`, text `*_scene.pbd`, the `objects/cut`
cutscenes) to FBX and back. The FBX opens in Blender, 3ds Max and Maya. Two UV sets (the `xt_*` materials need the
second), effect materials with their parameters, textures, LODs, ship and station scenes with animation, cameras with
targets.

[Русская версия](README.ru.md)

- **Lossless.** An unedited FBX comes back byte for byte: all 1 634 binary bodies of the game, 855 text bodies (in
  their binary form) and 949 scenes (AP 3.3). An unedited resave through Blender gives the original bytes too.
- **Edits stay local.** An edited part of a model gets its tangents and bounds recomputed, the other parts stay as
  they were. In a scene, editing an animation key changes exactly that key: TCB, flags and relative rotations are
  kept.
- **From the engine's code.** The formats and the animation interpolation (TCB, squad, camera targets) were worked
  out from `X3AP.exe` ([docs/scene_format.md](docs/scene_format.md), [docs/formats.md](docs/formats.md));
  `--bake-anim` shows the motion in the DCC the way the game computes it.

**Status.** The FBX round trip is checked over the whole game and through Blender 5.2. **Edited models and scenes have
not been tried in the game yet** — reports are welcome.

## Build

Windows, Visual Studio 2019 or later with the "Desktop development with C++" workload (it brings CMake and Ninja).
```
build.cmd
```
The result is `build\x3model.exe`. No external libraries: FBX is read with the bundled [ufbx](third_party/ufbx) and
written by an own binary FBX 7.4 writer (`src/fbx_write.cpp`); gzip is inflated by ufbx too. CI builds the exe (the
Actions tab, artifact `x3model`).

## Commands

| command | what it does |
|---|---|
| `x3model export <in> <out.fbx> [--no-textures] [--unit-mm N] [--bake-anim [fps]]` | body or scene → FBX. From the game, the textures (`dds/*.pck`, `tex/*.jpg`) are copied to `textures\` next to the FBX; a scene gets each part's body geometry under it. `--bake-anim` (scenes): frames of the engine's motion between the keys (30 per second by default) |
| `x3model import <in.fbx> <out.bob\|.pbb>` | FBX → body, or scene (when the FBX has a scene node) → `CUT1`; `.pbb` is written gzipped |
| `x3model info <in>` | a body's sections, LODs, materials, or a scene's parts (`X3_PART=n`: that part's keys) |
| `x3model pack <out.cat> [--force] <path inside>=<file> ...` | write a mod catalogue (`addon\NN.cat/.dat`); an existing one is replaced only with `--force` |
| `x3model verify <game folder>` | read and re-write every body and scene of every catalogue, compare the bytes |
| `x3model roundtrip-fbx <game folder> [filter]` | every body → FBX → body, compare the bytes |
| `x3model roundtrip-scenes <game folder> [filter] [--perturb] [--bake]` | every scene → FBX → scene, compare the `CUT1` bytes; `--perturb` edits the middle key of every animated part in the FBX (move, rotation or camera target) and checks that exactly that key changed; `--bake` goes through a baked FBX |
| `x3model derive-check <game folder> [filter]` | compare the computed tangents and part bounds with the game's |
| `x3model anim-sample <scene> [step ms]` | each part's motion from the engine evaluator (to compare with `tools/x3scene.py`) |

`<in>` is a file (`.bob .pbb .bod .pbd`) or a file in the game's catalogues: `"<game folder>|<path inside>"`, e.g.
`"C:\Games\X3 Terran Conflict|objects/ships/argon/argon_M3.pbb"` (the member the game reads: a loose file in the game
folder, else the latest catalogue, `addon` first).

## How to use

1. Export a model: `x3model export "C:\Games\X3 Terran Conflict|objects/ships/argon/argon_M3.pbb" argon_M3.fbx`.
2. Edit it in Blender (default FBX import; on export turn on **Custom Properties**, path mode Relative). Leave the
   `x3_*` properties of nodes and materials alone: they hold what FBX cannot. All LODs are visible and overlap; do not
   switch the extra ones off with "Disable in Viewports" (the monitor icon): Blender does not update such objects and
   exports them with a stale scale — that is why x3model does not mark LODs hidden.
3. Import it back: `x3model import argon_M3_edit.fbx argon_M3.pbb`. The import reports what it rebuilt and why.
4. Put it into a mod: `x3model pack "C:\Games\X3 Terran Conflict\addon\06.cat" objects/ships/argon/argon_M3.pbb=argon_M3.pbb`
   (the next free number: the game mounts catalogues in sequence up to the first gap; the new catalogue is the latest,
   and its `.pbb` overrides both the original `.pbb` and `.pbd`; `pack` will not overwrite a used number without
   `--force`). New textures go there as `dds/<name>.dds`.

A scene (a ship is a scene of bodies: `*_scene.pbd`) is exported the same way; its parts are nodes with their pose and
animation, their body geometry child nodes `…_geo` (for display: ignored on import). A new part is an empty node with
the property `x3_body` (the body name as in the scene's `B`, e.g. `ships\props\weapondummy`).

**Animation in Blender.** Blender reads FBX keys as linear and bakes the animation to frames on export. For an exact
result set **Bake Animation → Simplify = 0** on export: an unedited resave then gives the original bytes, and an edited
key changes exactly that key. With the default Simplify 1.0 Blender itself bends the curves by about 0.3 %; x3model
takes such places as unchanged within that tolerance, and once in a while a key falls outside it and is rewritten. The
"All Actions" option (on by default) does no harm: of the "object|action" stacks each object's own action is used.

- **Keys and the motion between them.** A normal export writes only the keys (easy to edit), and the DCC draws
  straight lines between them. In the game there is TCB between keys (Kochanek–Bartels with ease), squad for
  rotations, cameras aimed at targets. `--bake-anim` adds frames of that motion between the keys, computed by the
  engine's code, to see a cutscene as the game plays it. Such an FBX also comes back byte for byte; unchanged frames
  do not become keys on import, a changed frame becomes a new key.
- **Cameras with a target** (keys with flag `8`): the target is exported as a node `<camera>_target` beside the camera,
  with a key per target key. Move it — the target keys change on import. The camera's own rotation comes from the
  target and the roll and is not taken from the FBX; the FBX holds it as the engine computes it.
- **A key without its own position or rotation** (flags `0x10`, `0x20`) does not repeat the previous value: the engine
  interpolates the track at its time. It is exported that way, and after an edit of a neighbouring key such a key
  stays without a value.

## What is kept

**Unedited resave.** Points the FBX shows unchanged (position and UV to float32 precision, normal to 1 % of its
length) take back their stored values; faces Blender would drop (repeated corners, or the corners of another face) are
kept out of the mesh and restored from `x3_held_faces`.

**Edits are found per part.** Changed faces or face materials (the `x3_topology` fingerprint): the groups are rebuilt.
Any changed point, or a second UV set added or removed: that part's tangents and bounds are computed (as Egosoft's
exporter did: the u direction orthogonalised to the normal, and −v; the centre is the mean of the face vertices, see
`derive-check`); unchanged parts stay as they were. A changed vertex or face count, a copied object (the same
`x3_part`) or a shared point that differs between parts: the whole LOD is rebuilt — points by (vertex, normal, UV,
UV2), faces by material. Mirroring scale is handled. A mesh outside the LOD nodes goes to LOD 0. New materials without
`x3_*` get a `standard_lighting.fx` template with the textures of the FBX channels (diffuse, normal, specular,
emissive); materials keep their number through `x3_slot`, so renaming does not reorder them.

**Scenes**: part poses are taken relative to the nearest part ancestor (empties in between and the root scale Blender
adds on a resave are accounted for). Keys are merged one by one:
- a stored key the FBX shows unchanged stays byte for byte;
- a changed one gets the new pose in its own form: flags, TCB, colour, FOV, `extra` are kept, a relative rotation
  stays relative;
- relative keys after a changed one are encoded again so their own orientation does not move;
- an FBX key where there is no stored key and the neighbouring keys do not give the FBX pose becomes a new key with
  the flags and parameters of the previous one and an absolute rotation;
- for an FBX from Blender (frames baked) the value at a key's time is rebuilt as the corner between the frames
  around it.

A part copied in the DCC (the same `x3_order`) becomes a new part; parts are ordered so that a parent comes before its
children.

## Conventions

- Units: 1 game unit = 1 mm in the FBX (`--unit-mm` changes it; the unit is stored in `x3_unit_mm` and used on import).
  Body positions are normalised (up to 65536), the size is LOD 0's value; in the FBX they are already in game units,
  shared with the scenes.
- Axes: the game is left-handed (Direct3D), FBX right-handed, Y up: Z is mirrored; face corners are written (a, c, b).
- Scene part rotation: the engine's axis-angle keys (axis stored negated, angles in turns, accumulated without
  `0x2000`) apply to row vectors (as in Direct3D) — checked on a picture of the scene `dock9portsscene`.
- Key times: milliseconds (the sum of `dt`), seconds in the FBX.
- UV: v runs up in the FBX (down in the game).

## Limitations

- Text bodies with X2-era materials (`MATERIAL:`) and four `extern_*_dock` with a broken material record are not read
  (the game does not read them either). Numbers in text files are parsed with the engine's own code
  (`TextParseInt`/`TextParseFixed16`): a broken or missing number is an error, as for the engine.
- `MATERIAL3` (the game's global materials) are read, but have no texture in the FBX, and **such a body is converted to
  the binary form incorrectly**: the engine gives each face the nearest global material by colour at load time
  (`Material::FindNearestGlobal`), while the `.bob` gets the record number. `import` and scene builds with such an
  embedded body warn about it.
- A `MAT3`/`MAT5` body cannot get a new material (it holds only classic materials with global numbers): `import`
  refuses and asks to give the faces an existing one.
- A file saved without Custom Properties (x3model node names but no `x3_*`) is refused with a hint.
- `BONE`/`WEIG` (bones) are carried as data, not turned into an FBX skeleton (no body of the game has them).
- TCB parameters (tension, continuity, bias, ease) are not editable in the DCC: an edited key keeps them, a new key
  takes them from the previous one. Camera roll, FOV and light colour are likewise carried as key data only.
- The motion between keys in the DCC matches the game only with `--bake-anim`; the evaluator repeats the engine's
  16.16 code (checked against `tools/x3scene.py` on 797 843 samples), but the motion between keys was not compared
  with the running game.
- Position constraints (`CONS`/`K`: the position is taken from another part) are not shown in the FBX.
- Nested scenes (parts with flag `0x80`) and the dummy bodies of `types\Dummies` get no geometry.

## Checks

```
tests\run_checks.cmd "C:\Games\X3 Terran Conflict"
```
Reads and re-writes every body and scene of the installed game, sends them through FBX, edits keys and compares. The
expected numbers (in the script) are those of AP 3.3 from Steam. The `tests/blender_*.py` scripts check the round trip
through Blender (`blender -b --factory-startup -P tests/blender_resave.py -- <in.fbx> <out.fbx>`).

`tools/x3scene.py` is the Python reference reader of scenes (standard library only) and the animation evaluator the C++
was checked against: `python tools/x3scene.py check "<game folder>"`, `... interp "<game folder>"`.

## Documents

- [docs/formats.md](docs/formats.md) — bodies, text bodies, catalogues, the `x3_*` FBX properties, evidence from the exe;
- [docs/scene_format.md](docs/scene_format.md) — scenes: both file forms, keys, how the engine builds nodes, the exact
  interpolation.

## Legal

X3: Terran Conflict and X3: Albion Prelude are © Egosoft. This repository contains no game files and no game code;
x3model reads them from your own installation. The format descriptions are original work based on research of the
game.

Licensed under the MIT license, see [LICENSE](LICENSE). ufbx is MIT licensed (see [third_party/ufbx](third_party/ufbx)).
