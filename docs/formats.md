# X3 body and scene formats and how x3model converts them to FBX

Sources:
- `X3AP.exe`, read in a Ghidra database; addresses are VAs with image base `0x00400000`;
- the game data;
- x3-mod analyses, from the public repository [asvetliakov/x3-mod](https://github.com/asvetliakov/x3-mod):
  [`body-format-bob1.md`](https://github.com/asvetliakov/x3-mod/blob/HEAD/docs/reverse-engineering/body-format-bob1.md)
  and
  [`body-text-loader.md`](https://github.com/asvetliakov/x3-mod/blob/HEAD/docs/reverse-engineering/body-text-loader.md)
  (both in `docs/reverse-engineering/`);
- scenes: [scene_format.md](scene_format.md) in this repository and the reference parser
  [`tools/x3scene.py`](../tools/x3scene.py).

Tags: **[code]** - verified in the code, **[data]** - measured on the game files, **[inference]** - not checked
directly.

## Catalogs and packing

- **`.cat`:** every byte is XORed with `(0xDB + i)`. The result is text: the first line is the name of the `.dat`,
  then lines "path size"; the members lie in the `.dat` one after another, every byte XORed with `0x33`.
- **Compressed members** (`.pck`, `.pbb`, `.pbd`): gzip, or gzip with every byte XORed with the key
  `first byte ^ 0xC8` (`Archive::ResourceRead` `0x004e88e0`). Without these signs the data is taken as is.
  **[code]**
- **Mount order:** `NN.cat` from 01 up to the first gap, then `addon\NN.cat` in the same way; the one mounted
  later wins.
- **Choice of the body extension** (`pbb bob pbd bod`): decided by the topmost catalog that has the stem, and
  inside it by the order `.pbb` > `.bob` > `.pbd` > `.bod`.
- **The parser** is chosen by the content: `BOB`/`CUT` at the start means binary, otherwise text.
  **[code, x3-mod section 7 of body-format-bob1.md]**

## BOB1 body

Parser `Mesh::ParseBobBody` `0x00481aa0`, primitives `Body::Read*` (big-endian), tag table `0x0054ed50`. **[code]**

```
BOB1 { INFO cstr /INF | NAME cstr /NAM | VERS u32 /VER | SND1 u32 /SND
     | MAT3|MAT5|MAT6 i32 n material×n /MAT | BODY u16 nLod lod×nLod /BOD } /BOB
lod  := i32 value  u32 flags  [BONE i32 n cstr×n /BON]  POIN i32 n point×n /POI
        [WEIG i32 n (u16 k (u16 bone, i32 weight)×k)×n /WEI]  PART i32 n part×n /PAR
point:= u16 flags; 0x01: 3×i32 position; 0x02: 2×i32 UV 16.16; 0x04 (with 0x02): 2×i32 UV2; 0x08: 3×i32 normal 16.16;
        0x10: u32 (smoothing group of the source face)
part := u32 flags; u16 nGroups; group: i32 material, i32 nFaces, faces 4×i32 (a, b, c, word);
        if flags & 0x10000000: i32 n, n×(i32 point, 6×i32: two 16.16 vectors); after the groups 10×i32 of bounds
```

**Materials**
- **MAT6 with flag `0x02000000`** is an effect material (`Material::ReadEffectMaterial` `0x00470490`): u16
  technique, effect string, u16 number of parameters, then for each: name, u16 type (`SPTYPE_LONG` 0 … `STRING`
  8), value.
- **Classic MAT6:** texture string, 14×u16, u16, 5×(string, u16).
- **MAT3/MAT5:** u16 textures, 14×u16, u16 flags, u16, 3×(u16, u16).

The field order was checked against the decompilation. **[code]**

**Meaning of the fields**
- **Positions** are normalised: the largest-magnitude coordinate is about 65536. The size of the object is the
  value of LOD 0, and one file unit equals `value/65536` game units. This agrees with the scenes: the ship
  `argon_M3_scene` assembled in Blender matches part by part. **[data]**
- **Tangent records** - one per point of the group, in the order in which the point first appears in the faces of
  the group (matches in 13 263 groups of 13 266). The first vector is the direction of growth of u,
  orthogonalised to the point normal (to within one unit of the lowest digit on the points checked). The second
  is roughly the direction of decrease of v; the exact averaging of the 3ds Max exporter was not reproduced.
  **[data]**
- **The 10 bounds numbers of a part:**
  - the centre - the integer mean of the vertices of the faces, with truncation; matches in 1451/1234/1163 parts
    of 1774 for x/y/z;
  - the L-infinity radius from the centre;
  - the AABB centre - (min+max)/2;
  - the AABB half-sizes - (max−min)/2.

  The AABB matches in 72-90% of the parts; the rest are apparently float roundings of the exporter. **[data]**
- **Face corner order:** in a right-handed system after the Z mirror the point normals look against the order
  (a, b, c), so a face is written to FBX as (a, c, b). Verified: in Blender, 10 645 of 10 647 faces of `argon_M3`
  looked against the normals with the direct order and 2 with the reversed one. **[data]**
- **Game normals** are often far from unit length (thousands): that is how the data is, and they are carried over
  as they are.

**Check:** `verify` - 1 671 of 1 672 bodies byte for byte. The only failure is
`Bothers_khaak_hive_stations.pbb`: it is truncated in the shipped game. One body (`weapons/terran_mine.pbb`) is
broken - the LOD 0 value is `INT_MIN`, positions are zero; its scale is 1. **[data]**

## Text body (.bod/.pbd)

Read like the engine's `Mesh::ParseTextBody` `0x00483f20` (x3-mod body-text-loader.md):
- values end with `;`, and `/` opens a comment up to the end of the line, so the blocks `/! N: … !/` are not
  read;
- a LOD record is a value, then the vertices up to `-1;-1;-1;`, then the parts up to `-99`, then the LOD flags
  (binary digits);
- normalisation is `(v<<16)/m`, where `m` is the largest-magnitude coordinate of the record;
- a face has 3 or 4 corners (a fan), the word is `(−flags) & ~1`;
- points are merged when they have the same smoothing group, the same position and |ΔUV| < 2;
- normals are the sum of the face normals weighted by the corner angles, and flat for group 0;
- there are no tangents.

Normals are computed in float, not exactly as in the engine's fixed point. The materials `MATERIAL3/5/6` are
supported. **[code x3-mod, data]**

**Check:** 865 text bodies are read. The failures are 6 bodies with X2 materials (`MATERIAL:`) and 4
`extern_*_dock`, where MATERIAL6 is written in the MATERIAL3 layout. **[data]**

## Scenes (CUT1 and text)

The full description is in [scene_format.md](scene_format.md). In short:

```
CUT1 := "CUT1" "INFO" cstr "/INF" i32 version i32 nParts part×n "/CUT"
part := "PATH" u32 P (version<6 ? i32 : cstr) body i32 C i32 F u32 flags ["NAME"…] ["CONS"…] ["BOB1"… embedded body]
        i32 nNotes ["NOTE"…] i32 nKeys ["STAT" (u32 key flags, fields by flags)×n "/STA"] "/PAT"
```

- **Text scene:** `VER: n;`, then `P n; B body; <header letters> { key } …`. The lexer is the same as for bodies.
- **INFO** is mandatory: without it `Scene::LoadBinaryCut` fails. x3model writes `INFO` even when it converts to
  `CUT1` a text scene that has none. **[code]**
- **Key** (`Scene::ReadCutKeyframes` `0x00490650`): position i×3, then one of the rotation variants - axis-angle
  f×4 (the angle in turns, the axis with a minus sign), three angles, or target/roll; then TCB, colour, FOV, `dt`,
  `extra`. The key time is the sum of the preceding `dt` in ms.
- **Rotation:** without `0x2000` the keys are relative, `q = q_prev ⊗ q_key`. **[code]**
- **Applying the rotation:** the engine applies it to a row vector - in FBX this is the conjugate quaternion,
  followed by the Z mirror (x, y with a minus sign). Verified by a picture: the five docking arms of
  `stations/docks/dock9portsscene` converge to the centre as a correct five-pointed star, whereas with the
  opposite convention they fly apart. **[data]**
- **Motion between keys** ([scene_format.md](scene_format.md), section 9; `src/scene_anim.cpp` is its copy and
  matches the reference `tools/x3scene.py` on 797 843 samples): position - Hermite with Kochanek-Bartels tangents
  and the ease curve when the start key of the segment has `0x4000`, otherwise linear; rotation - slerp or squad
  (`0x8000` on either of the two keys); camera target - yaw and pitch from the direction to the target, roll from
  the roll track; a key without its own position/rotation is interpolated. **[code]** It was not compared with
  the game itself.
- **The body of a part** is given by a name or a number. A number is looked up in `types\BodyData` (file name or
  `v\%05d`). Embedded bodies (`BOB1` in `CUT1`, `L { … }` in text) have the number `cut·100000+nn`; x3model
  compiles the text ones into `BOB1` when writing `CUT1`.

**Check:**
- `verify`: `CUT1` 54/54 byte for byte;
- text scenes: 1 081 of 1 083, and 2 are rejected by the game itself (`06001.pbd` with decimal commas and
  `06243.pbd` with `#QNAN`); conversion to `CUT1` gives the same tree;
- `roundtrip-scenes`: 949/949 byte for byte. **[data]**

## x3_* properties in FBX

| where | properties |
|---|---|
| body root | `x3_kind=body`, `x3_sections` (section order), `x3_info`, `x3_name`, `x3_vers`, `x3_snd`, `x3_mat_version`, `x3_lods`, `x3_unit_mm` (mm per game unit), `x3_text_mat3` (a text body with `MATERIAL3`) |
| LOD node | `x3_lod`, `x3_value` (size/threshold), `x3_flags`, `x3_points`, `x3_bones`, `x3_weights` |
| body part node | `x3_part`, `x3_flags`, `x3_groups` (material:number of faces …, including the held ones), `x3_vertex_count`, `x3_face_count` (faces in the mesh), `x3_topology` (FNV-1a of the corners and materials of the mesh faces), `x3_point_index`, `x3_point_data` (12 int32 per point: flags, smoothing, position, normal, UV, UV2 - as in the file), `x3_face_words`, `x3_held_faces` (faces outside the mesh: the number in the part list and three LOD points), `x3_tangents`, `x3_bounds` |
| material | `x3_slot` (place in the body array), `x3_index`, `x3_flags`, `x3_kind` (`effect`/`classic`), `x3_technique`, `x3_effect`, `x3_params` (strings "name TYPE value", FLOAT - decimal), `x3_classic` |
| scene root | `x3_kind=scene`, `x3_binary`, `x3_version`, `x3_info`, `x3_unit_mm`, `x3_bake_fps` (an FBX with baked motion) |
| camera target | `x3_target_of` (the `x3_order` of the camera) |
| scene part | `x3_part` (P), `x3_order`, `x3_body`, `x3_mode` (C), `x3_parent` (F), `x3_flags`, `x3_name`, `x3_constraints`, `x3_notes`, `x3_embedded`, `x3_inline`, `x3_keys` (all keys as they are) |
| body geometry under a part | `x3_ref` (for display only, skipped on import) |

Binary data is stored in base64 (int32, little-endian); corrupted or short data is an import error. If the
geometry or animation in the FBX does not match the stored data, that data is not used and is recomputed. The
import reports this line by line: which LOD or which part was rebuilt and why (`X3_BODY_DEBUG=1` and
`X3_SCENE_DEBUG=1` show the first mismatches).
