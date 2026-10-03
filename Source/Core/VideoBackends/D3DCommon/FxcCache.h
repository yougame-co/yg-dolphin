// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"

// Orca (ORCA.md, "Match start"): what FXC compiled, kept across Orca builds.
//
// Dolphin's compiled-shader files (Cache/Shaders/D3D-*.cache) carry the build's commit, so every
// Orca update compiled every shader again before the first frame, through glslang, SPIRV-Cross and
// FXC (d3dcompiler_47.dll). FXC is most of that time (the 63 ubershaders alone took about 35 s on
// Luis's PC) and a pure function of its input, so this keeps its output keyed by exactly
// that input: the HLSL text, the target, the flags, the macros and a SHA-1 of the compiler DLL's
// bytes. A build whose shader generators write the same HLSL finds its shaders here; any change to
// the text, the compile or the compiler is another key, so a binary can only ever be used for the
// very compile that made it. Cache/Shaders/D3D-fxc.cache, one file for every game; sessions and
// plain Dolphin alike, when Dolphin's shader cache is on.
namespace D3DCommon::FxcCache
{
using Key = Common::SHA1::Digest;

// The key of one compile; empty when the cache is off (Dolphin's shader cache setting, or a
// compiler DLL that couldn't be read).
std::optional<Key> MakeKey(std::string_view hlsl, std::string_view target, u32 flags,
                           std::string_view macros);

// FXC's output for this key from an earlier compile, of this run or an earlier one.
std::optional<std::vector<u8>> Find(const Key& key);

// Keeps a compile's output (in memory and appended to the file).
void Store(const Key& key, std::span<const u8> bytecode);

// Logs what the cache did and closes its file (UnloadLibraries).
void Close();
}  // namespace D3DCommon::FxcCache
