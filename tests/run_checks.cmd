@echo off
rem Full acceptance over the installed game (about 2.5 min): every body and scene re-written byte for byte, every body
rem and scene through FBX and back. Usage: tests\run_checks.cmd ["D:\GAMES\X3 Terran Conflict"]
setlocal
set GAME=%~1
if "%GAME%"=="" set GAME=D:\GAMES\X3 Terran Conflict
set X=%~dp0..\build\x3model.exe
echo == verify (expected: BOB1 1671 identical + 1 truncated, CUT1 54, text bodies 865 + 10 refused, text scenes 1081 + 2 refused)
"%X%" verify "%GAME%"
echo == roundtrip-fbx (expected: identical 2489, error 11: the truncated body and the 10 refused text bodies)
"%X%" roundtrip-fbx "%GAME%"
echo == roundtrip-scenes (expected: identical 949, error 2: the two text scenes the engine refuses)
"%X%" roundtrip-scenes "%GAME%"
echo == roundtrip-scenes --perturb (expected: edited parts 2882, as expected 2882)
"%X%" roundtrip-scenes "%GAME%" --perturb
