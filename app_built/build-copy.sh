#!/bin/sh

# copy
cp ../build/app.exe .
cp ../build/_deps/sdl3-build/SDL3.dll .
cp ../build/_deps/sdl3_mixer-build/SDL3_mixer.dll .

# check
deplister app.exe
