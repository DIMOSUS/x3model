#pragma once
// Scenes: binary CUT1 (.bob/.pbb starting with "CUT1") and text scenes (.bod/.pbd starting with "VER:"), read into
// one tree, written as CUT1. Layout and engine semantics: docs/formats.md, "Scenes" (from Scene::LoadBinaryCut
// 0x00490ec0, Scene::LoadTextCut 0x00491b10, Scene::ReadCutKeyframes 0x00490650; reference implementation
// RETools projects/x3ap/model/x3scene.py).
//
//   CUT1 := "CUT1" "INFO" cstr "/INF" i32 version i32 nParts part*n "/CUT"
//   part := "PATH" u32 index (version < 6 ? i32 body : cstr body) i32 mode(C) i32 parent(F) u32 flags
//           ["NAME" cstr "/NAM"] ["CONS" i32 n (i32, i32)*n "/CON"] ["BOB1" <body> "/BOB"]
//           i32 nNotes ["NOTE" (i32 time, cstr)*n "/NOT"] i32 nKeys ["STAT" (u32 flags, key)*n "/STA"] "/PAT"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace x3::scene {

// Fields of a key record, in file order, from its flag word (Scene::ReadCutKeyframes).
enum FieldKind { kInt, kFixed };
struct Field {
  const char* name;
  FieldKind kind;
};
std::vector<Field> KeyLayout(uint32_t flags);

struct Key {
  uint32_t flags = 0;
  std::vector<int32_t> values;  // in KeyLayout(flags) order
  int32_t Get(const char* name) const;  // 0 if absent
  bool Has(const char* name) const;
};

struct Part {
  uint32_t index = 0;        // P number (the key F refers to)
  std::string body;          // body reference as written (CUT1 version < 6: the number as text)
  int32_t mode = 0;          // C
  int32_t parent = -1;       // F
  uint32_t flags = 0x40;
  std::optional<std::string> name;
  std::optional<std::vector<std::pair<int32_t, int32_t>>> constraints;  // CONS / K
  std::vector<std::pair<int32_t, std::string>> notes;
  std::vector<uint8_t> embedded;   // CUT1: "BOB1 ... /BOB" bytes of an embedded body
  std::string inlineText;          // text scene: the 'L { ... }' text body (converted to BOB1 on writing CUT1)
  std::vector<Key> keys;
};

struct Scene {
  bool binary = true;  // read from CUT1 (false: text)
  int32_t version = 6;
  std::optional<std::string> info;
  std::vector<Part> parts;
  std::vector<uint8_t> tail;  // bytes after /CUT (none in the shipped files)
};

bool IsCut1(const std::vector<uint8_t>& data);
bool IsTextScene(const std::vector<uint8_t>& data);
Scene ReadCut1(const std::vector<uint8_t>& data);
Scene ReadText(const std::vector<uint8_t>& data);
Scene Read(const std::vector<uint8_t>& data);
// CUT1 bytes; a text scene's inline bodies are compiled to embedded BOB1 bodies
std::vector<uint8_t> WriteCut1(const Scene& sc);

// time (ms) of every key: 0, then the running sum of the previous keys' dt
std::vector<int64_t> KeyTimes(const Part& p);

}  // namespace x3::scene
