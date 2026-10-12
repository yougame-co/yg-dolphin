// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <picojson.h>

// A strict JSON reader for game data files (Orca split S5: rulesets first). It accepts exactly what
// the site's parseStrictJson (YouGame web/src/lib/orca-pack-validate.ts) accepts, so a file the site
// takes is a file Orca takes:
//   - one RFC 8259 value and nothing after it but space, tab, CR and LF;
//   - the whole text valid UTF-8, strings without raw control characters, \u escapes with paired
//     surrogates only;
//   - numbers in JSON's grammar (no leading zeros, no "1.", no "+1"), kept as doubles as picojson
//     does (exact up to 2^53);
//   - no key twice in one object;
//   - at most `max_depth` levels of values (the root is level 1, as the site counts), and at most
//     `max_bytes` bytes, so recursion and memory are bounded before anything is read.
// Pure: no locale, no allocation beyond the value it builds, the same result on every host.
namespace Orca::Engine
{
std::optional<picojson::value> ReadStrictJson(std::string_view text, size_t max_bytes,
                                              int max_depth, std::string* error);
}  // namespace Orca::Engine
