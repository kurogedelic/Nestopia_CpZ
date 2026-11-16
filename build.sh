#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Leo Kuroshita (@kurogedelic)
# Builds out/nestopia-cz with g++ (needs libsdl2-dev for sound). Run on arm64 Debian 13 (the Zero itself or a container).
set -e
cd "$(dirname "$0")"
N=third_party/nestopia/source; mkdir -p out/obj
find $N/core -name '*.cpp' | xargs -P"$(nproc)" -I{} sh -c 'o=out/obj/$(echo {} | tr / _).o; [ -f $o ] || g++ -std=gnu++14 -O2 -w -DNST_NO_ZLIB -c {} -o $o'
g++ -std=gnu++14 -O2 -w -DNST_NO_ZLIB -DNESTOPIACZ_HAS_SDL -I$N/core -I$N/core/api -Isrc $(sdl2-config --cflags) src/main.cpp out/obj/*.o $(sdl2-config --libs) -o out/nestopia-cz
echo out/nestopia-cz
