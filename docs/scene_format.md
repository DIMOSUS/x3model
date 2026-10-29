# X3: Albion Prelude scenes: binary `CUT1` and the text format

The goal is to give the converter (scene <-> FBX node hierarchy with animation) the complete grammar of both
kinds of scene file and the way the engine turns them into nodes. The source of truth is `X3AP.exe` (read in a
Ghidra database; addresses are VAs with image base `0x00400000`), then the game data (`01..13.cat`,
`addon\01..04.cat`; `addon\05.cat` is the author's own mod and was not counted).

Tags: **[code]** - verified in the code (decompiler/disassembler, address given nearby); **[data]** - measured on
the installed files (`python tools/x3scene.py check "<game folder>"`, scripts in section 10); **[inference]** -
follows from code or data but was not checked directly. **[x3-mod]** links point to third-party research in the
public repository [asvetliakov/x3-mod](https://github.com/asvetliakov/x3-mod), directory
[`docs/reverse-engineering/`](https://github.com/asvetliakov/x3-mod/tree/HEAD/docs/reverse-engineering) (not
re-checked unless stated).

The reference implementation is [`tools/x3scene.py`](../tools/x3scene.py) (Python 3, standard library only): it
reads both kinds into one tree, writes the tree back as `CUT1` and as text, and runs the acceptance check over
all installed files.

## 1. Loading: which file and which parser

| address | what it does |
|---|---|
| `Scene::LoadCutScene` `0x00492970` | cache by scene id (hash at `g_pRenderManager+0x80`) and a failure cache (`IsCutLoadFailed`/`MarkCutLoadFailed`); the name comes from `types\CutData` (`GetCutSceneName`) or is `cut\%05d`, the path is `objects\<name>`; `Archive::OpenStream` with the extension list **`pbb bob pbd bod`** and the tag table `Mesh::g_Cut1TagTable` `0x0054eed0` (0xb entries); if the payload starts with **`CUT`** -> `Scene::LoadBinaryCut` `0x00490ec0`, otherwise -> `Scene::LoadTextCut` `0x00491b10` **[code]** |
| `Scene::BuildPart` `0x0043d1d0` | scenes of ships/stations from the types (`TShips` etc.) and the "dummy" scenes `types\Dummies`: builds nodes part by part **[code]** |
| `Scene::InstantiateCut` `0x0045f270` | cutscenes (`cut\NNNNN`): nodes, cameras, light sources, real game objects **[code]** |

The extension does not select the parser: only the `CUT` prefix decides. File selection uses the same resolver
as for bodies (`Archive::ResolveName` `0x004e7590`): a loose file first, then the catalogs from the top down,
and inside a layer `.pbb > .bob > .pbd > .bod` **[inference]** (the resolver order was verified in x3-mod for
bodies, [body-format-bob1.md][bob1] section 7; scenes use the same `OpenStream` with the same list - **[code]**).
Hence, of the 6 stems that have both a `CUT1` and a text file, the game always reads the `CUT1` (section 8).

The gzip/XOR wrapper of `.pbb/.pbd` is removed before parsing (`Archive::ResourceRead` `0x004e88e0`; the wrapper
is described in [formats.md](formats.md)).

## 2. Common stream primitives

These are the same functions as for `BOB1` bodies (`Body::*`). Stream object: `+0x08` cursor, `+0x14` tag table,
`+0x18` entry count, `+0x1c` binary mode, `+0x24` error flag. **[code]**

| function | binary mode (big-endian) | text mode |
|---|---|---|
| `Body::ReadTag` `0x004e9b70` | 4 bytes; a match with a tag (`+4`) -> entry index, with the closing tag (`+8`) only for the expected index -> `count` (=0xb). **A miss: error flag set, cursor not advanced, returns `count-1` (=10)** - this is how the loader tests optional tags | compares with the text labels of entries 2..9 (`NAME:`, `VER:`, `PATH:`, `STATES:`, `NOTES:`, `POSCONSTRAINT:`, `BODY:`, `INFO:`) |
| `Body::ReadI32` `0x004ea610` | i32 | `Body::TextParseInt` `0x004eb930` |
| `Body::ReadU32` `0x004ea660` | u32 | `TextParseUnsigned` |
| `Body::ReadRawI32` `0x004eafc0` | i32 (16.16) | `Body::TextParseFixed16` `0x004ec490` |
| `Body::ReadString` `0x004ea1b0` | string up to NUL, cursor advances `len+1` | `Body::TextReadToken` `0x004e9930` up to `;` |
| `Body::TextReadCharInSet` `0x004ea160` | - | skip whitespace, return the character, **consume it only if it is in the set** |
| `Body::SeekTagInStream` `0x004e9e20` | byte-by-byte search from the cursor for the string of table entry `+0xa8` (for `CUT1` this is `/BOB`) | - |

The text lexer **[code]**:

- **The buffer's NUL overwrites the file's last byte** (`Scene::LoadTextCut` `0x00491b4b`: `MOV byte ptr
  [ECX+EAX-1],0`), it is not added after it. A file that ends in the last key's `}` with no newline loses the
  brace, the `}` check (`0x0049260e`) fails and the whole scene is refused. The stock files end in CRLF.

- `TextSkipWhitespace` `0x004e98a0`: skips TAB, LF, CR and space; **a single `/` starts a comment up to the end of
  the line** (so `/ ...`, `// ...` and `/====` are all comments; `/` is not allowed in names).
- `TextParseInt`: optional `-`; `%` means binary, `0x` hexadecimal (with a minus sign - an error), otherwise
  decimal; **a number ends only at `;` (consumed) or NUL**; whitespace and line breaks inside are skipped
  (`1 2;` = 12); a leading `#...;` is the value 0 without an error; any other character is an error.
- `TextParseFixed16`: digits, one `.`, a leading `-`; accumulation in double (`v = (c + v*10) - 48`, fraction
  `scale *= 0.1; v += d*scale`, the constants are the doubles at `0x00565530/38/40`), then
  `_ftol(v*65536.0)` - **truncation toward zero**; the end is `;` or NUL. A comma, an exponent or `#QNAN` after
  digits is an error. x87 precision is assumed to be 53 bits (otherwise rare differences of 1/65536 appear)
  **[inference]**; `x3scene.py` computes in double with the same order of operations.
- `TextReadToken` (strings): up to `;` (consumed) or NUL; CR/LF/TAB are dropped, spaces inside are kept, `/`
  starts a comment.

## 3. Binary `CUT1`

The tag table `Mesh::g_Cut1TagTable` `0x0054eed0` (entries of 0x14 bytes: description, tag, closing tag, text
label, index) **[code]**:

| index | tag | closing | description | text label |
|---|---|---|---|---|
| 0 | `DATA` | `/DAT` | - | - |
| 1 | `CUT1` | `/CUT` | - | - |
| 2 | `NAME` | `/NAM` | Name | `NAME:` |
| 3 | `VERS` | `/VER` | Version | `VER:` |
| 4 | `PATH` | `/PAT` | Path | `PATH:` |
| 5 | `STAT` | `/STA` | States | `STATES:` |
| 6 | `NOTE` | `/NOT` | Notes | `NOTES:` |
| 7 | `CONS` | `/CON` | Position constraint | `POSCONSTRAINT:` |
| 8 | `BOB1` | `/BOB` | Embedded body | `BODY:` |
| 9 | `INFO` | `/INF` | Info | `INFO:` |
| 10 | - | - | (empty, the "miss") | - |

Grammar (`Scene::LoadBinaryCut` `0x00490ec0`, all numbers big-endian) **[code]**:

```
file  := "CUT1" "INFO" cstr "/INF" i32 version i32 nParts part×nParts "/CUT"
part  := "PATH" u32 index
         ( version < 6 ? i32 bodyId : cstr bodyName )
         i32 mode(C) i32 parent(F) u32 partFlags
         [ "NAME" cstr "/NAM" ]
         [ "CONS" i32 n (i32 partNumber, i32 value16_16)×n "/CON" ]
         [ "BOB1" <BOB1 body> "/BOB" ]
         i32 nNotes [ "NOTE" (i32 time, cstr text)×nNotes "/NOT" ]     ; the tag is present only if nNotes ≠ 0
         i32 nKeys  [ "STAT" (u32 keyFlags, key(keyFlags))×nKeys "/STA" ] ; the tag is present only if nKeys ≠ 0
         "/PAT"
```

- `INFO` is effectively mandatory: after `CUT1` the loader calls `ReadTag(1)`; anything other than `INFO` leaves
  the error flag set and the load fails **[code]**. The string is the exporter's passport (`$PATH: … SRC: ….max
  VERS: 3.38 TIME: … Copyright … EGOSOFT$`) **[data]**.
- `VERS` is not used in `CUT1`: the version is a bare i32 after `/INF`. Versions in the data: 6 - 51 files,
  4 - 3 files **[data]**.
- The optional `NAME`/`CONS`/`BOB1` are probed with `ReadTag(4)`; a miss (index 10) clears the error flag at
  `0x0049157f` **[code]**. The order is rigid: NAME, CONS, BOB1.
- `CONS` with n = 0 occurs (`06271.pbb`, 4 parts) and must be preserved for a byte-exact round trip **[data]**.
- **Embedded body** (`0x0049151c`): id = `bodyId + (cut − 1)·100000`; it is parsed by
  `Mesh::ParseBobBodyFromBuffer` `0x00482f00` starting at the address of the `BOB1` tag (so it is an ordinary
  `BOB1` file, see [body-format-bob1.md][bob1]); then `SeekTagInStream` searches for the **first** `/BOB` after the
  tag - the body boundary is found by a string search, not by parsing; if the body data contained `/BOB`, the
  engine would go wrong **[code]**. If the part has a `NAME`, it is copied into the body at `+0x4c`. The data
  contain 1215 embedded bodies, with body names of the form `1000nn` **[data]**.
- Part flags on load **[code]**: `version < 6` -> `|= 0x40`, the body is an id; `version ≥ 6`: `flags & 0x80` ->
  the name is a **scene** (`Body::NameToId` `0x0046e690`, the scene table, ids 50000+), otherwise `|= 0x40` and
  `Mesh::GetOrRegisterBody` `0x0046e400` (the body table; a string of digits is the id itself).
  `flags & 0x200` -> scene flag `cut+0x54 |= 4`.
- No file has a tail after `/CUT` **[data]**.

## 4. Text scene

`Scene::LoadTextCut` `0x00491b10` **[code]**; the character sets are the strings at `0x0056124c..0x0056126c`
(`P`, `B`, `CFENKMLTclbjfsp`, `{`, `d`, `}`) and `0x0055589c` `p`, `0x00555880` `s`.

```
file   := comments "VER:" int ";" part* <the rest is not read>
part   := "P" int ";" "B" token ";" header* key*
header := "C" int ";"                       mode -> part +0x38 -> node +0x260
        | "F" int ";"                       parent: the P number of the parent, default −1
        | "N" token ";"                     part name
        | "K" int n ";" (int ";" fixed ";")×n      position constraint (= CONS)
        | "T" (int ";" token ";")* "-1;"     notes (= NOTE), at most 100
        | "M" (fixed ";")×9                 matrix: read and discarded
        | "L" ( "{" text "}" | text )       embedded text body (without braces: up to the first '{')
        | "c"                               flags |= 1
        | "l" ["d" | "p" | "s"]             flags |= 2 (+4 | +8 | +0x10): light source
        | "f"                               flags |= 0x20
        | "j"                               flags = flags & ~0x40 | 0x80: B is a scene name
        | "p"                               flags |= 0x100 (animation repeat)
        | "s"                               flags |= 0x200 and cut+0x54 |= 4
        | "b"                               nothing
        | "E"                               end of header (consumed)
key    := "{" [ int flags ";" ] fields(flags) "}"     ; for VER < 2 there is no flags word, flags = 1
```

Details:

- Initial part values: body −99999, C 0, F −1, flags **0x40**, everything else 0 **[code]**.
- The header continues while the next character (after skipping whitespace and comments) is in the set
  `CFENKMLTclbjfsp`; a character outside the set is not consumed and ends the header. The letter order is free.
  A `p` immediately after `l` is the light type, not the repeat flag.
- `B` is always a string; after the header: flag 0x80 -> `NameToId`, otherwise, if there was no `L`,
  `GetOrRegisterBody(B)` **[code]**.
- `L` (`0x004920ec..`, addresses from the x3-mod plate): id = `GetOrRegisterBody(B) − 100000 + cut·100000` (for
  such parts `B` is a number of the form `1000nn`); with `{`, the text up to the first `}` is parsed by the text
  body loader (`Mesh::ParseTextBodyFromBuffer`, if the body is not loaded yet); without `{`, up to the first `{`
  (which remains the start of the first key) **[code]**. The data contain only the braced variant, 142 parts in
  30 files (starting with `06001.pbd`) **[data]**.
- Keys are read while the next character is `{`; after a key a `}` is mandatory, otherwise the whole scene fails
  to load (returns 0) **[code]**.
- After the keys of a part: if the next character is `P`, a new part starts; **any other character ends the
  scene without an error**. A part with no `B` after `P n;` sets the error flag, but the parts already read are
  kept **[code]**.
- Exporter comments (`// 1` after a key is a repeat of the "extra" field, `// idx 0`, `/ Node … Class […]`) are
  not read by the engine.

Example (`06.cat objects\ships\usc\terran_m3p_scene.pbd`):

```
VER: 3;
P 1; B ships\props\turretdummy3; C 3; N Bships\props\turretdummy3_3a; b
  { 0x2002;  0; -730; -8353;  0.500; 1.000; 0.000; 0.000;  3333;  1; } // 1
  { 0x2002;  0; -730; -8353;  0.500; 1.000; 0.000; 0.000;  -1;  -1; } // -1
```

## 5. Key record: the flags word and fields

`Scene::ReadCutKeyframes` `0x00490650`; the caller passes `flags | 0x80` **[code]**. Fields in order (i =
`ReadI32`, integer; f = `ReadRawI32`, 16.16, a decimal fraction in text):

| condition | fields | destination | meaning |
|---|---|---|---|
| `!(f & 0x10)` | i x, y, z | position track `+0x20..` | position in engine units (500 = 1 m [x3-mod, game-scale]) |
| ... and `f & 0x4000` | f ×5 | `+0x08..+0x18` | position TCB: tension, continuity, bias, ease in (to), ease out (from) |
| `!(f & 8)`, `f & 0x20` | - | with `f & 2`: an identity rotation key, flag 0x20 is cleared | "no rotation" |
| `!(f & 8)`, `f & 2` | f angle, ax, ay, az | `+0x20` angle, `+0x30..+0x38` **axis with a minus sign** | axis-angle, the angle in turns (1.0 = 360°) |
| ... and `f & 0x8000` | f ×5 | `+0x08..+0x18` | rotation TCB |
| `!(f & 8)`, `!(f & 2)`, `!(f & 0x200)` | f a, b, c | `+0x20..+0x28` | three angles (turns, the low 16 bits are taken) |
| `f & 8`, `!(f & 0x40)` | i tx, ty, tz | target track | the point the node looks at (a camera) |
| `f & 8`, `!(f & 0x20)` | f roll | roll track, **with a minus sign** | roll about the view direction, in turns |
| `f & 0x200`, `!(f & 0x400)` | f r, g, b | colour track | light colour, 1.0 = 255 |
| `f & 0x800`, `!(f & 0x1000)` | f fov | FOV track, `(v<<16)/0x6487e` | field of view in **radians** (0x6487e = 2π·65536), stored in turns |
| always | i dt, i extra | `g_CutKeyTime += dt` | time to the next key; `extra` is **not used** by the engine |

- **Key time** = the sum of the `dt` of the preceding keys of the part (the first one is 0); the `dt` of the last
  key in the data is always −1 (27 641 of 27 643 text parts, all 3 061 `CUT1` parts) **[code + data]**. The unit
  is **milliseconds** **[inference]**: `Scene::AnimateNode` computes speed as `Δpos·1000/(dt+10)`, and the node
  time is taken from the game clock `EngineConfig+0x718`.
- `extra`: most often the number of the next key (i+1: 75 % of `CUT1` keys, 61 % of text keys), −1 for the last
  key, in the others time/100 and the like **[data]**; the engine reads and discards it **[code]**.
- Flag 0x2000: the rotation is absolute; without it the rotation keys are **relative** and accumulate (section
  6). Flag 4 is set by the engine itself (`BuildRotationKeyQuats`); a key from a file with bit 4 and without 0x20
  would not be converted to a quaternion **[code]**. `ReadCutKeyframes` does not test bit 1 or the bits above
  0x8000 (a `TEST` instruction listing for the function shows: 0x10, 0x4000, 8, 0x40, 0x20, 2, 0x8000, 0x200,
  0x400, 0x800, 0x1000) **[code]**.
- Limit: at most 20 000 keys per track, otherwise an error **[code]**.
- Frequent flag words **[data]**: `CUT1` - 0x2003 (206 076), 0xc082, 0x40a2, 0x2002, 0xd882, 0x3802 …; text -
  0x2002 (76 152), 0xc082, 0x2003, 0x2082, 0x2, 0x40a2, 0x12 … Decoding: 0x2002 - position and absolute
  axis-angle; 0xc082 - position with TCB and relative axis-angle with TCB; 0x3802 - absolute axis-angle, FOV
  declared (0x800) and suppressed (0x1000). Bit 0x80 is often already set in the file.

## 6. How the engine builds nodes and animation

**Scene definition** (0x60 bytes, `LoadBinaryCut`/`LoadTextCut`) **[code]**: `+0x00` scene id, `+0x04` part
count, `+0x50` array of parts of 0x58 bytes each, `+0x54` flags (4 = some part has flag 0x200).

**Part record** (0x58 bytes; for text it is assembled in `Scene::g_CutPartStaging` `0x00609958`, up to 0x1000
parts) **[code]**:

| offset | contents |
|---|---|
| `+0x00/+0x04` | count and array of position keys (0x50 bytes: `+0` time, `+4` flags, `+8..+0x18` TCB, `+0x20` value, `+0x30`/`+0x40` tangents or axis/quaternion) |
| `+0x08/+0x0c` | rotation keys (0x50) |
| `+0x10/+0x14` | target keys (0x50) |
| `+0x18/+0x1c` | roll keys (0xc) |
| `+0x20/+0x24` | colour keys (0x14) |
| `+0x28/+0x2c` | FOV keys (0xc) |
| `+0x30` | body id |
| `+0x34` | part number P |
| `+0x38` | C |
| `+0x3c` | F |
| `+0x40` | name |
| `+0x44/+0x48` | notes {i32 time, char*} |
| `+0x4c` | part flags |
| `+0x50/+0x54` | constraints {i32 part number, 16.16} |

`Scene::CommitCutArrays` `0x004901a0` moves the tracks, calling `BuildRotationKeyQuats` `0x0048f760` and
`ComputeTcbTangentsForKeys` `0x0048fe40` beforehand **[code]**.

**Hierarchy** **[code]**:

- `F` is the `P` number of the parent. `BuildPart` looks for the parent in the already built subtree
  (`FindChildNodeBySegmentKey` `0x00486fd0`: a node with the same scene id and a record whose `+0x34 == F`),
  and `AttachPartChildren` `0x0043d160` re-attaches, after the build, children whose parents came later.
  `InstantiateCut` searches only among the parts created earlier. Without `F` (−1) the part is the scene root (an
  object or a sector node). In the data F refers only to earlier parts (641 of 641) **[data]**.
- The P number is a key, not a position: in 3 `CUT1` files and 111 text files the numbers are not consecutive
  **[data]**.
- The node gets `+0x258` scene id, `+0x25c` part index, `+0x260` = **C** (`StampNodeSceneIds` `0x0048eb40`),
  then `AnimateNode(node, cut, 0)` - the pose at time 0 **[code]**.

**C is the mode/group word of the part** (`node+0x260`): for nozzles - the main-nozzle bit (1), visibility
(0x1000), "no trail" (0x2000), "no effect" (0x4000), steering (0x4..0x80) - **[x3-mod]**
[engine-effects.md][engine-effects]; the mask `0xffe` and an arbitrary mask in
`Scene::PickRandomSubtreeVertex` `0x0047f560` **[code]**. The most frequent values: 0, 1, 2, 3, 0x10, 0x7001,
0x4 … **[data]**.

**What the body of a part is** (by id) **[code]**:

| id | `BuildPart` (object scenes) | `InstantiateCut` (cutscenes) |
|---|---|---|
| −1 | the part is skipped | **camera**: node with `animFlags \|= 0x1000`, FOV goes to the camera |
| −2 | light source | (ordinary path) |
| 1000..1999 (BuildPart) / 1000..3999 or flag `l` (0x2) | light from the light table (`g_pRenderManager` + id·0x18) | light; 2000..2999 - with flag `0x20000000` (`lensParam` 0), 3000..3999 - point light, attenuation (1, 10, 0), range 1 000 000 |
| 4000..9999, 50000..79999 | - | a real game object (type by value); 9999 is the player's ship, 124 and 99999 are special |
| an entry of `types\Dummies` | dummy: a nested scene from `CutData`, guns, docks ([ship-scene-parts.md][ship-scene-parts]) | - |
| other | model (`SetNodeModel`) | model, or a game object if some type has such a body |
| `cut·100000 + nn` | body embedded in the scene (`BOB1` / `L`) | the same |

Part flag 0x20 (`f`) -> `node.flags |= 0x2000000` on the whole subtree (`InstantiateCut`) **[code]**. Flags 1
(`c`) and 4/8/0x10 (`ld`/`lp`/`ls`) are not read by the engine in these functions; the letters d/p/s are
naturally read as directional/point/spot **[inference]**.

**Animation** (`Scene::AnimateNode` `0x0048dc20`, time t in ms from the start) **[code]**:

- A key is found by bisection (`FindKeyframeIndex` `0x00496d30`); before the first - the first, after the last -
  the last. The animation length is the largest time of the last key over all tracks
  (`GetAnimationLength` `0x0048cec0`).
- **Position**: with flag 0x4000 - Hermite with Kochanek-Bartels tangents (TCB: `ComputeTcbKeyTangents`
  `0x0048f850`, ease - `TcbEaseCurve`), otherwise linear. The values are the local position `node+0x30`.
- **Rotation**: keys with flag 2 are quaternions: `q = AxisAngle(angle, −axis_file)` (the half angle is taken as
  `angle/2 & 0xffff` in 16-bit turns); when 0x2000 is **absent**, `q_i = q_{i−1} ⊗ q_rel_i` (the order of
  factors is `Math::QuatMultiplyFixed` `0x004f2640`, x = w1x2 + x1w2 + y1z2 − z1y2, the first factor is the
  accumulated one). Interpolation: if either of the two keys has 0x8000 - TCB "squad"
  (`InterpolateRotationKeyTcb` `0x0048d2a0`), otherwise slerp; the matrix is `Math::QuatToMatrixRows` `0x004f2300`
  (the standard `1−2(y²+z²), 2(xy−wz), 2(xz+wy) …`). Keys without flag 2 are three angles, linear interpolation
  of the raw values without taking the shortest arc, the matrix is `Math::AnglesToMatrix(a, b, c)` `0x004f0270`;
  by the formulas (`m00 = cos c·cos a + sin c·sin b·sin a`) and by the camera code, a is yaw (about Y), b is
  pitch (X), c is roll (Z) **[code]**, exactly - section 9.
- **Target** (flag 8): yaw and pitch are computed from the direction "position -> target" through an arctangent
  table, roll comes from the roll track (linear along the shortest 16-bit arc).
- **Colour**: only for light nodes (`node+0x12c & 4`), 16.16·255 -> bytes `node+0x150..0x154`.
- **FOV**: for a camera (`+0x298`) or for a node linked to a camera.
- **Position constraint** (CONS/K): if present, the node position is taken from the world position of the node
  with part number `cons[0].partNumber` (the second value of the pair is not used in this function).
- `ComposeAttachTransform` `0x0048ce40` converts the local pose into a pose relative to the parent.
- **Repeat**: part flag 0x100 (`p`), scene flag 4 (`s`) or `node+0x1f4 & 0x800` - time modulo the length;
  otherwise at the end the node is marked finished (`+0x1f4 |= 0x200`) and the script callback is called.
- **Notes**: `Scene::ParseCutSegmentMarkers` `0x0048ec90` looks for `start N`/`end N` (`lstart`/`lend` - with
  repeat, `end speak` is skipped) - segments of the cutscene by number N. Words in the data: `start`, `end`,
  `event …`, `camera …` **[data]**; the consumers of `event`/`camera` were not traced.

## 7. Mapping to FBX (recommendations for the converter)

- Part -> node; the name is `N` (otherwise the body), the parent is the part with `P == F`, otherwise the root.
  Keep `P`, `C`, the part flags, the original body string and `extra` in custom properties - without them the
  round trip cannot be closed.
- Positions are integer engine units (500 per metre) [x3-mod, game-scale]; FBX frames come from the sum of `dt`
  (ms).
- Rotation: build quaternions as `x3scene.rotation_quats()` (axis sign, turns, accumulation without 0x2000); when
  writing back, use relative keys `q_rel = q_{i−1}⁻¹ ⊗ q_i`, or absolute ones with 0x2000. The handedness and
  the order of multiplying node matrices with the parent were not compared with a picture in the game
  **[inference]**: check it on one scene (the M3P turret `0.5; 1; 0; 0` -> 180° about X).
- TCB (5 numbers per key) has no direct analogue in FBX: keep it as properties or bake the curve.
- Embedded bodies: `CUT1` stores the whole `BOB1` (see [body-format-bob1.md][bob1]), text stores a text body in
  `L { … }`; `x3scene.py` keeps them as opaque bytes/text.

## 8. Reference implementation and acceptance

`x3scene.py` repeats the loader code step by step (the lexer is a copy of `TextParseInt`/`TextParseFixed16`/
`TextReadToken`); the tree: `Scene` (form, version, info, parts, head/tail) -> `Part` (index, body, mode, parent,
flags, name, constraints, notes, embedded, inline, matrix, keys) -> `Key` (flags, values by `key_layout(flags)`).
The text writer keeps the spelling of numbers if the value was not changed; for new 16.16 values it picks a
decimal spelling that the engine (with truncation) reads back to the same value.

Run (`python tools/x3scene.py check "C:\Games\X3 Terran Conflict"`, about 35 s, all catalog members, not only the
winners) **[data]**:

| check | result |
|---|---|
| `CUT1`: parse -> write byte for byte | **54 / 54** |
| `CUT1` -> text -> tree (scenes without embedded `BOB1`) | 10 / 10 the same tree |
| text scenes (starting with `VER:`) | 1083; parsed as in the engine: **1081**; 2 are rejected by the engine as well (below) |
| text -> text -> tree | 1082 / 1082 the same tree; byte for byte 0 (comments are not preserved); after stripping comments and whitespace it matches 1081 / 1082 (`06001.pbd` - commas, read only in the lenient mode) |
| text -> `CUT1` -> tree | 1082 / 1082 (except 142 embedded text bodies in 30 files: for `CUT1` they need a text -> `BOB1` converter, which `x3scene.py` lacks) |

Two text files that the engine does not load either **[code + data]**:

- `01.cat objects\cut\06001.pbd`: numbers with a **comma** (`0,694444`) - `TextParseFixed16` sets an error.
  The same catalog has `06001.pbb` (`CUT1`), and that one is read; `x3scene.read_text(strict=False)` reads it.
- `01.cat objects\cut\06243.pbd`: `-1.#QNAN0` in the FOV field and keys with flag **0x20000** and five extra TCB
  numbers after the target (the format of a newer exporter; `ReadCutKeyframes` does not know such a flag).
  There is no `06243.pbb`, so cutscene 6243 will not load in the game **[inference]**; whether it is used was not
  checked.

Stems with both kinds (6 pairs) - **no pair describes the same tree**; they are different revisions **[data]**:

| pair | parts CUT1 / text | matching parts (by body and name) |
|---|---|---|
| `01 cut\06001` | 4 / 6 | 0 |
| `05 cut\06011.pbb` / `01 …pbd` | 104 / 64 | 1 exactly, 21 within 1/65536 |
| `05 cut\06012` / `01` | 377 / 16 | 0 |
| `05 cut\06013` / `01` | 15 / 8 | 0 |
| `01 others\khaak_hive_scene` | 9 / 9 | 5 (4 bodies renamed to `…_scene`; integer coordinates differ by 1) |
| `02 ships\M4\Split_m4_pirate_scene` | 26 / 24 | 22 (16 within 1/65536) |

`CUT1` is newer everywhere (VERS 3.53, X3.5) and wins by the resolver rule (the same or a higher layer).

Census of the data **[data]**: `CUT1` versions 6 ×51, 4 ×3; text 3 ×921, 6 ×144, 2 ×16; up to 377 parts in
`CUT1` and 1456 in text; kinds of bodies - names, ids `<1000` (`BodyData`), 10000..19999 (`Dummies`), 1000..3999
(lights), −1 (camera, 136 + 187), 4000..9999 (object types), `cut·100000+nn` (embedded); header letters:
`Nb` 14 745, `CNb` 9 719, `N` 1 722, … ; `M` does not occur; `CUT1` part flags: 0 ×1418, 0x40 ×976, 0x80 ×517,
0x1 ×136, 0x4a, 0x42, 0xa.

## 9. Interpolation (exact)

What `Scene::AnimateNode` `0x0048dc20` computes from the key tracks, to the bit where the engine is integer. This
section refines the "Animation" list of section 6. Reference implementation: `x3scene.engine_tracks`,
`evaluate_tracks`, `evaluate`, `sample` (16.16 integers mirrored; the engine's x87 code done in doubles, so a
result may differ by 1/65536 in rare cases). Tags as above: **[code]** / **[data]** / **[inference]**.

Notation (all 16.16 unless said): `r16(a,b) = (a·b + 0x8000) >> 16` (64-bit product, arithmetic shift: `IMUL;
ADD 0x8000; ADC 0; SHRD 16`); `scale(k, v)` = `r16` per component (`Math::Vec3ScaleFixed` `0x0040e780`);
`fdiv(a,b) = trunc(a·65536/b)`, 0 when b = 0 (`Math::FixedDivide` `0x00412450`); integer `/` truncates toward
zero (`IDIV`); `trunc()` of a float is `Crt::Ftol`. Angles in 16-bit turns (0x10000 = 360°); `fπ` = float π =
3.1415927410125732.

### 9.1 Tracks and key search

- `ReadCutKeyframes` sends each record to independent tracks (table in section 5); all fields of one record
  share its time (running sum of `dt`). Position keys come from **every** record without 0x10, including
  target-mode (flag 8) records. Rotation keys: records without 8; with 0x20 only if 0x2 (identity key: angle 0,
  axis (−1,0,0), 0x20 cleared); else 0x2 -> axis-angle; else (no 0x200) -> three angles **[code]**.
- Fields a record does not carry are **0**: the staging arrays are zero-filled (`Util::TrackedRealloc` zero-fills
  growth; `AllocCutKeyArrays` allocates from size 0 for every part) and `CommitCutArrays` copies them into
  `memset`-cleared blocks. So a key without 0x4000/0x8000 has T = C = B = ease = 0, and target keys have zero
  tangents **[code]**.
- Per track at time t (`FindKeyframeIndex` `0x00496d30`, `…12`/`…20` for the short records): i = last if
  t ≥ t_last; 0 if t ≤ t_0; else bisection to t_i ≤ t < t_{i+1}. If t == t_i or i is the last key, the key's
  value is used as is, else segment i -> i+1 is interpolated **[code]**. t is clamped to ≥ 0 and t_0 = 0, so
  "before the first key" does not occur; after the last key every track holds its last value.
- Evaluation order: position -> rotation -> target -> roll -> colour -> FOV; each of rotation, target and roll
  rewrites the node matrix `node+0x40`, the last one wins **[code]**.

### 9.2 Position tangents (`ComputeTcbTangentsForKeys` `0x0048fe40`)

Computed by `CommitCutArrays` for all position keys, whether or not they have 0x4000. `+0x30` = TS (incoming,
used at the end of the segment that ends at the key), `+0x40` = TD (outgoing) **[code]**.

- n = 1: both zero. n = 2: `TD_0 = scale(1−T_0, P_1−P_0)`, `TS_1 = scale(1−T_1, P_1−P_0)`.
- n ≥ 3: `TS_0 = TD_last = 0`. Interior key i (`ComputeTcbKeyTangents` `0x0048f850`), with
  `d_in = P_i − P_{i−1}`, `d_out = P_{i+1} − P_i` (integers):

  ```
  TS_i = scale(fdiv(t_i − t_{i−1}, t_{i+1} − t_{i−1}),
               scale(r16(r16(1−T, 1+C), 1−B), d_out) + scale(r16(r16(1−T, 1−C), 1+B), d_in))
  TD_i = scale(fdiv(t_{i+1} − t_i, t_{i+1} − t_{i−1}),
               scale(r16(r16(1−T, 1−C), 1−B), d_out) + scale(r16(r16(1−T, 1+C), 1+B), d_in))
  ```

  This is Kochanek-Bartels with the usual time adjustment 2Δt/(Δt_in+Δt_out); the ½ of the weights and the 2 of
  the adjustment cancel.
- End keys (`ComputeTcbStartTangent` `0x0048fb60`, `ComputeTcbEndTangent` `0x0048fcd0`, the same arithmetic):

  ```
  f(D, d, r, T) = scale(½, D) + ( scale(1−T, scale(¼ − r, D)) + scale(1.5, d − scale(½, D)) )
  TD_0    = f(P_2 − P_0,       P_1 − P_0,       fdiv(t_1 − t_0, 2(t_2 − t_0)),       T_0)
  TS_last = f(P_L − P_{L−2},   P_L − P_{L−1},   fdiv(t_L − t_{L−1}, 2(t_L − t_{L−2})), T_L)
  ```

  With even spacing and T = 0 that is `1.5d − 0.25D`. C and B of the end keys are not used.

### 9.3 Ease (`Scene::TcbEaseCurve` `0x0048cf60`)

`ease(s, ein, eout)`, s ∈ [0, 1]; the callers pass **ein = ease out (+0x18) of the segment's start key** and
**eout = ease in (+0x14) of its end key** **[code]**:

```
if ein + eout == 0: return s
if ein + eout > 1:  ein = fdiv(ein, sum); eout = fdiv(eout, sum)
k = 2^32 / (0x20000 − ein − eout)            // 1/(2 − ein − eout), 0 if the divisor is 0
s < ein:        k · r16(s, s) / ein          // 64-bit, truncated
s < 1 − eout:   r16(k, 2s − ein)
else:           1 − k · r16(1−s, 1−s) / eout  // eout == 0 -> 1
```

### 9.4 Position between keys (`Scene::InterpolatePositionKey` `0x0048d0c0`)

`s = fdiv(t − t_i, t_{i+1} − t_i)`. **Only key i (the segment start) decides** **[code]**:

- key i has 0x4000: `e = ease(s, easeout_i, easein_{i+1})`, `e2 = r16(e,e)`, `e3 = r16(e2,e)`,
  `h00 = 2e3 − 3e2 + 1`, `h01 = 3e2 − 2e3`, `h10 = e3 − 2e2 + e`, `h11 = e3 − e2`;
  `P = ((scale(h01, P_{i+1}) + scale(h00, P_i)) + scale(h10, TD_i)) + scale(h11, TS_{i+1})`.
- otherwise linear: `P = scale(s, P_{i+1}) + scale(1 − s, P_i)`.

The result is integer engine units (parent space). Key i+1's flag does not matter, but its TS and ease in are
used. Keys without 0x4000 inside a TCB track (614 parts) have zero T/C/B, so their own tangents are
Catmull-Rom-like **[data]**. A hold before a jump overshoots: `01 cut\04799.pbb` part 148 holds still for 54 ms
and then jumps 10.5·10⁶ units; the hold segment swings out by 94 033 units **[data + code]** (engine behaviour
by the code, not watched in game). Also computed: velocity `node+0x100 = ΔP·1000/(Δt+10)` per segment, zeroed
after the last key.

### 9.5 Rotation keys to quaternions (`BuildRotationKeyQuats` `0x0048f760`)

Only keys with 0x2 and without 4 and 0x20; afterwards bit 4 is set on every key. A file key with bit 4 keeps
q = (0,0,0,0), which gives the identity matrix **[code]**.

- `q_rel = AxisAngleToQuatFixed` `0x004f2b50`: axis (as stored, i.e. negated) all zero -> (0x10000,0,0,0)
  exactly; else `h = (angle / 2) & 0xffff` (C division), `w = cos16(h)`, `xyz = r16(axis, sin16(h))`. The axis
  is not normalised (140 axis-angle keys in the data have |axis| off by more than 1 %) **[code + data]**.
- `sin16`/`cos16`: one table `T[k] = trunc(sin(k·f2π/65536)·65536)`, k = 0..0x4000, with quadrant folding
  (`Math::BuildAsinTable` `0x004f0110`, f2π = float 2π). Hence `cos16(0) = T[0x4000] = 65535` **[code]**.
- Without 0x2000: `q_i = QuatMultiplyFixed(q_rel, q_{i−1}) = q_{i−1} ⊗ q_rel`. That is 16 `r16` products, then
  `QuatNormalizeFixed`: `k = 1/(|q|·2⁻¹⁶)` in floating point, each component `trunc(c·k)`. With 0x2000:
  `q_i = q_rel`, not normalised. The running value starts at identity **[code]**.
- Drift: one step differs from the float product by at most 7.3·10⁻⁵ (396 907 keys), but the errors accumulate.
  After hundreds of relative keys the matrix differs from the float accumulation (`rotation_quats`) by up to
  0.0073 per entry (`01 cut\00948.pbb` part 10 and others) **[data]**.

### 9.6 Rotation between keys

If **neither** key of the segment has 0x2, see 9.8. Otherwise `InterpolateRotationKeyTcb` `0x0048d2a0` is
called; it clamps t to [0, t_{i+1}] and takes `s = fdiv(t − t_i, t_{i+1} − t_i)` **[code]**.

- Neither key i nor key i+1 has 0x8000: `q = slerp(q_i, q_{i+1}, s)`.
- Otherwise squad (Shoemake form with KB tangents). The **log vectors come from the keys' own axis-angle fields,
  not from the quaternions**: `L_k = (0, r16(axis_k, m_k))`, `m_k = trunc(θ·65536)`, `θ = angle_k·2⁻¹⁶·fπ` (half
  angle, rad), and `θ − fπ` when θ ≥ fπ/2, i.e. exactly when angle_k ≥ 0x8000 (comparisons checked in the
  disassembly). The angle is not masked to 16 bits here. For a relative key `L_k = log(q_{k−1}⁻¹ q_k)`.

  ```
  L_i   : if i == 0 then L_i = L_{i+1}, adj_i = 1, else
          r = (t_i − t_{i−1})·0x20000 / (t_{i+1} − t_{i−1})          // int64, trunc; 0 if /0
          adj_i = C_i − r16(2 − r, C_i) + (2 − r)                    // = (1−C)(2−r) + C
  L_{i+2}: if i+2 is past the last key then L_{i+2} = L_{i+1}, adj_{i+1} = 1, else
          r' = (t_{i+1} − t_i)·0x20000 / (t_{i+2} − t_i)
          adj_{i+1} = C_{i+1} − r16(r', C_{i+1}) + r'                // = (1−C) r' + C
  wa = r16(r16(r16(1−T_i, 1+C_i), 1+B_i) / 2, adj_i)
  wb = r16(r16(r16(1−T_i, 1−C_i), 1−B_i) / 2, adj_i)
  g  = (r16(wa, L_i) + r16(wb − 1, L_{i+1})) / 2                     // per component, trunc
  a_i = q_i ⊗ exp(g)                                                 // QuatMultiplyFixed(exp g, q_i)
  wa' = r16(r16(r16(1−T_{i+1}, 1−C_{i+1}), 1+B_{i+1}) / 2, adj_{i+1})
  wb' = r16(r16(r16(1−T_{i+1}, 1+C_{i+1}), 1−B_{i+1}) / 2, adj_{i+1})
  h  = (r16(1 − wa', L_{i+1}) + r16(−wb', L_{i+2})) / 2
  b_{i+1} = q_{i+1} ⊗ exp(h)
  e  = ease(s, easeout_i, easein_{i+1})
  q  = slerp(slerp(q_i, q_{i+1}, e), slerp(a_i, b_{i+1}, e), 2·r16(e, 1 − e))
  ```

  `exp(v)`: `m = trunc(sqrt(float32(x²+y²+z²)))`, `θ = m/65536`, `w = trunc(cos θ·65536)`; if
  `|float32(sin θ)| < 0.001` then `xyz = v`, else `xyz = v·trunc(float32(sin θ)·65536) / m` (int64, trunc).
- `slerp(from, to, u)` (`Math::QuatSlerp` `0x004f2920`, ECX = to, EDI = from, ESI = out): the components ·2⁻¹⁶ as
  doubles; `dot = x·x' + y·y' + z·z' + w·w'`. dot > 1 -> θ = 0; dot < 0 -> use −dot and subtract the "to" term
  (**shortest arc**); `θ = acos`; sin θ == 0 -> "from"; else
  `trunc((sin((1−u)θ)/sin θ · from ± sin(uθ)/sin θ · to)·65536)`. The u < 0 / u > 1 copies at the start have no
  `RET` and are overwritten; harmless, since u ∈ [0, 1] here **[code]**.
- Matrix (`Math::QuatToMatrixRows` `0x004f2300`), rows, every product `r16`:
  `[(½−zz−yy)·2, (xy−wz)·2, (wy+xz)·2]`, `[(wz+xy)·2, (½−zz−xx)·2, (yz−wx)·2]`,
  `[(xz−wy)·2, (wx+yz)·2, (½−yy−xx)·2]`, applied to row vectors **[code]**.
- **Quirk (candidate bug):** for a key with 0x2000, L_k is the absolute rotation, not the step from the previous
  key, so the squad tangents near such a key are wrong. The first key is harmless (L_0 is never used). In the
  data 608 TCB rotation tracks have an absolute key later than the first. Example: `01 cut\00948.pbb` part 23,
  key 1 (0xe082): the step is 1.1°, but the absolute value is 116.7°, so L_1 is about 100× too long. That
  affects segments 0->1 and 1->2. **[code + data]**, the effect was not seen in game.

### 9.7 `Math::AnglesToMatrix(m, a, b, c)` `0x004f0270`

Each angle `& 0xffff`, then `·2⁻¹⁶·f2π` (x87), entries `trunc(·65536)` **[code]**:

```
row 0:  cc·ca + sc·sb·sa      sc·cb      cc·sa − sc·sb·ca
row 1:  cc·sb·sa − sc·ca      cc·cb      −sc·sa − cc·sb·ca
row 2:  −sa·cb                sb         cb·ca
```

(`sc·sb`, `cc·sb` and `sb` are spilled to float32 first.) For row vectors (`v' = v·M`):
`M = Rz(c)·Rx(b)·Ry(a)` with `Ry(a) = [[ca,0,sa],[0,1,0],[−sa,0,ca]]`, `Rx(b) = [[1,0,0],[0,cb,−sb],[0,sb,cb]]`,
`Rz(c) = [[cc,sc,0],[−sc,cc,0],[0,0,1]]`. So roll is applied first, then pitch, then yaw. **a = yaw about Y,
b = pitch about X, c = roll about Z**; local +Z (row 2) = (−sa·cb, sb, ca·cb). In terms of `QuatToMatrixRows`:
`M = QuatToMatrixRows(qz(−c) ⊗ qx(b) ⊗ qy(a))`, where `q_axis(φ) = (cos φ/2, axis·sin φ/2)` and ⊗ is the operand
order of section 6. Both identities were checked numerically on 2 000 random triples (max 1.5·10⁻⁵) **[data]**.

### 9.8 Three-angle keys and `Math::LerpInt` `0x00469de0`

Between two keys without 0x2: a, b, c each `LerpInt(t − t_i, t_{i+1} − t_i, v_{i+1}, v_i)` on the raw 16.16
values (**no shortest arc**), then `& 0xffff` -> `AnglesToMatrix`. No shipped scene has three-angle keys (0
parts) **[data]**.

`LerpInt(t, D, end, start)`: D == 0 -> (start+end)/2; t < 1 -> start; t ≥ D -> end; t < 0x10000 and D < 0x10000
-> `(start+end)/2 + MulDiv(end−start, 2t−D, 2D)` (Win32 `MulDiv`, rounds half away from zero); else
`(start+end)/2 + (end−start)(t − D/2)/D` (int64, trunc) **[code]**.

### 9.9 Target mode (flag 8)

- Target point G: the target key, or `InterpolatePositionKey` over the target track. It would be Hermite if the
  target key had 0x4000, with zero tangents and ease (smoothstep), but no shipped target key has 0x4000
  **[code + data]**. G is compared with `node+0x30` as just computed by the position track: **the same (parent)
  space as the position keys** (with CONS/K: the other node's world position) **[code]**.
- `u = P.x − G.x`, `v = G.z − P.z`; `yaw = atan16(u, v)`; `hor = r16(v, cos16(yaw)) + r16(u, sin16(yaw))`;
  `pitch = atan16(G.y − P.y, hor)`. Stored as `node+0x200 = yaw`, `+0x204 = pitch` (16 bits); the matrix is
  `AnglesToMatrix(yaw, pitch, node+0x208)` **[code]**.
- `atan16(y, x)` = angle of (x, y) from +x towards +y, by octant, with
  `A[i] = trunc(atan(i/65536)/(8·atan 1)·65536)` (`Math::g_AtanTable` `0x005e69a8`, 0x10001 entries, 0..0x2000,
  FISTP with chop) and `i = fdiv(small, big)` **[code]**:

  ```
  y < 0, x < 0:  x < y ? A[fdiv(−y,−x)] + 0x8000 : 0xc000 − A[fdiv(−x,−y)]
  y < 0, x ≥ 0:  −y < x ? (−A[fdiv(−y,x)]) & 0xffff : A[fdiv(x,−y)] + 0xc000
  y ≥ 0, x < 0:  y < −x ? 0x8000 − A[fdiv(y,−x)] : A[fdiv(−x,y)] + 0x4000
  y ≥ 0, x ≥ 0:  y < x ? A[fdiv(y,x)] : (x == y == 0 ? 0 : 0x4000 − A[fdiv(x,y)])
  ```

  Then row 2 of the matrix = (G − P)/|G − P|, i.e. the node looks along local +Z. On the 203 target keys in the
  data the direction is off by at most 0.37° (table precision) **[data]**.
- Roll track (value stored negated): at a key `r & 0xffff`; between keys `d = (r_{i+1} − r_i) & 0xffff`,
  `d −= 0x10000` if `d > 0x8000` (**shortest 16-bit arc**), `roll = LerpInt(t − t_i, t_{i+1} − t_i, r_i + d, r_i)`
  (not masked; AnglesToMatrix masks). It is stored at `+0x208` and gives
  `AnglesToMatrix(node+0x200, node+0x204, roll)` **[code]**.
- Mixing: the target and roll tracks run after the rotation track and rewrite the matrix whenever they have any
  key. A part with target keys therefore ignores its rotation keys at all times. Without a roll track the roll
  is whatever `+0x208` holds: the c angle of the rotation track, the **y component of a quaternion**, or the
  previous frame's value. A roll track without target keys (flag 8 | 0x40 keys only) takes yaw/pitch from
  `+0x200/+0x204` as the rotation track left them **[code]**. In the data: 103 parts with target keys, 101
  cameras (body −1) and 2 lights (body 1374); none has rotation keys, every one has a roll track; 3 mix in
  8|0x40 keys (no target point) **[data]**.

### 9.10 FOV and colour

- FOV: read as `(v·65536)/0x6487e` (int64, trunc) = turns in 16.16 (90° -> 16383, not 16384); `LerpInt` between
  keys; written to the camera `+0x298` when `node+0x12c & 1`, or to the linked camera `*(node+0x1f8)+0x298` when
  `node+0x1f4 & 0x1000` **[code]**.
- Colour: `LerpInt` per channel on the 16.16 values, then `byte = (v·255 + 0x8000) >> 16` ->
  `node+0x150/0x152/0x154`; only nodes with `node+0x12c & 4` **[code]**.

### 9.11 Time, end, loop

`Scene::GetAnimationLength` `0x0048cec0` = max(0, time of the last key of each non-empty track) **[code]**.
AnimateNode takes `t = now − start` (≥ 0) and `rest = node+0x1ec − t`, where `+0x1e8`/`+0x1ec` are the play
window. With node flag 0x8000, `t = node+0x1e8 + rest` (played backwards) **[inference]**. Without
0x2000/0x8000, `rest` is measured to the animation length when that is shorter. When `rest < 1`: with part flag
0x100, scene flag 4 or `node+0x1f4 & 0x800` and a positive window, the start is moved so that t continues
modulo the window `+0x1ec − +0x1e8` (not necessarily the animation length). Otherwise `+0x1f4 |= 0x200`, and
with `+0x1f4 & 0x40` the script callback runs once **[code]**. A converter that previews one pass evaluates
t ∈ [0, length] and repeats for looping parts.

### 9.12 Self-check

`python tools/x3scene.py interp "C:\Games\X3 Terran Conflict"` (about 30 s) runs over every part that has TCB
(0x4000/0x8000) or target (8) keys. Results **[data]**:

| check | result |
|---|---|
| parts | 3 762 (target parts 103) |
| position at the 81 124 position-key times = the key value | all |
| rotation at 82 137 key times vs `rotation_quats` (float, normalised after each product) | max 0.0073 per matrix entry; fixed-point accumulation, one step ≤ 7.3·10⁻⁵ |
| orthonormality of all sampled matrices | max 4.7·10⁻⁴ |
| target keys (203): row 2 vs the direction to the target | max 0.37° |
| `AnglesToMatrix` vs `QuatToMatrixRows(qz(−c) ⊗ qx(b) ⊗ qy(a))`, 2 000 random triples | max 1.5·10⁻⁵ |

Not checked against the running game: the motion between keys (squad, ease, Hermite) is the code read
statically. Velocity, constraints and the play window are not modelled in `evaluate`.

## 10. Reproduction

- Acceptance: the command above; `python tools/x3scene.py dump <file>`, `tocut1`, `totext`.
- Interpolation: `python tools/x3scene.py interp <game folder>` (section 9.12); `evaluate(part, t)`,
  `sample(part, step)`; decompile with Ghidra, in the database of X3AP.exe, the functions `0x0048dc20 0x0048d0c0
  0x0048d2a0 0x0048cf60 0x0048fe40 0x0048f850 0x0048fb60 0x004f2920 0x004f0270 0x004f2300 0x004f2b50
  0x004f0110`.
- Decompilation (Ghidra, the database of X3AP.exe) of `0x00492970 0x00490ec0 0x00491b10 0x00490650 0x0048f760
  0x0048dc20 0x0045f270 0x0043d1d0 0x0048ec90 0x004e9b70 0x004e98a0 0x004eb930 0x004ec490 0x004e9930`; the tag
  table is 55 pointers at `0x0054eed0`; the key flags are found by listing the `TEST`/`AND` instructions of
  `0x00490650`.
- Names in the database: `Scene::g_CutPartStaging*` (`0x00609958..0x006099ac`), `g_CutRollKeyCapacity`
  `0x006085a0`, `g_CutNoteStaging` `0x00609638`; plates (comments) on `LoadCutScene`, `LoadBinaryCut`,
  `LoadTextCut`, `ReadCutKeyframes`, plus a supplement to `AnimateNode`.

## 11. Open questions

- The time unit (ms) is an inference from `·1000/(dt+10)`, not a measurement in the game.
- `AnglesToMatrix` and `QuatToMatrixRows` are reconciled exactly (section 9.7); in the game only the application
  of the matrix to a row vector was verified; the order of the camera angles (yaw/pitch/roll) is by the code.
- The meaning of part flag 1 (`c`), of 4/8/0x10 and of key bit 1: they are not read in the traced functions;
  other readers of `part+0x4c` were not searched for.
- The second value of the CONS/K pair (16.16) is not used in `AnimateNode`; whether other readers exist was not
  checked.
- The consumers of the notes `event …`/`camera …`.
- x87 precision when parsing text fractions (53 bits assumed); if D3D switches the FPU to 24 bits, differences of
  1/65536 are possible.
- Converting embedded text bodies (`L`) to `BOB1` for writing `CUT1` is not implemented.

[bob1]: https://github.com/asvetliakov/x3-mod/blob/HEAD/docs/reverse-engineering/body-format-bob1.md
[engine-effects]: https://github.com/asvetliakov/x3-mod/blob/HEAD/docs/reverse-engineering/engine-effects.md
[ship-scene-parts]: https://github.com/asvetliakov/x3-mod/blob/HEAD/docs/reverse-engineering/ship-scene-parts.md
