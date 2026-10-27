@echo off
rem Full acceptance over the installed game (a few minutes): every body and scene re-written byte for byte, every
rem body and scene through FBX and back, single-key edits, baked animation.
rem Usage: tests\run_checks.cmd "C:\Games\X3 Terran Conflict"   (or set X3_GAME). The expected numbers are those of
rem X3: Albion Prelude 3.3 as installed from Steam (TC + AP); another install gives other counts.
setlocal
set GAME=%~1
if "%GAME%"=="" set GAME=%X3_GAME%
if "%GAME%"=="" (
  echo usage: tests\run_checks.cmd "^<game folder^>"   or set X3_GAME
  exit /b 2
)
set X=%~dp0..\build\x3model.exe
echo == verify (expected: BOB1 1671 identical + 1 truncated, CUT1 54, text bodies 865 + 10 refused, text scenes 1081 + 2 refused)
"%X%" verify "%GAME%"
echo == roundtrip-fbx (expected: identical 2489, error 11: the truncated body and the 10 refused text bodies)
"%X%" roundtrip-fbx "%GAME%"
echo == roundtrip-scenes (expected: identical 949, error 2: the two text scenes the engine refuses)
"%X%" roundtrip-scenes "%GAME%"
echo == roundtrip-scenes --perturb (expected: edited parts 2882, as expected 2882)
"%X%" roundtrip-scenes "%GAME%" --perturb
echo == roundtrip-scenes --bake (expected: identical 949, error 2)
"%X%" roundtrip-scenes "%GAME%" --bake
