cmake_minimum_required(VERSION 3.20)
project(x3model CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(x3model
  src/main.cpp
  src/archive.cpp
  src/bob.cpp
  src/body_fbx.cpp
  src/derive.cpp
  src/fbx_write.cpp
  src/textbody.cpp
  src/scene.cpp
  src/scene_fbx.cpp
  src/scene_anim.cpp
  src/ufbx_impl.cpp
)
target_include_directories(x3model PRIVATE src third_party/ufbx)
if(MSVC)
  target_compile_options(x3model PRIVATE /W4 /utf-8 /permissive-)
  target_compile_definitions(x3model PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  set_source_files_properties(src/ufbx_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
  set_property(TARGET x3model PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
