// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "Core/Orca/UX/Kit.h"

// The YouGame layer (design F, Luis 2026-10-05): what every YouGame game shares is drawn in
// YouGame's own look instead of the game's, the way Steam's overlay draws its chat over any game.
// For now that is the chat and the room's notices (joined, left, a game's result). The look is the
// cards the SDK shows inside YouGame's browser games (web/public/sdk.js: slate #0f172a cards, text
// #cbd5e1, the site's avatar colours), in Roboto (the site's face; Data/Sys/Orca/Fonts, Apache 2.0),
// which ImGui rasterizes at any size. Local display only, like the kit: nothing here reads emulated memory.
namespace Orca::UX::Yg
{
// Loads the cards' face into ImGui's atlas now (once; cheap after): the overlay calls it every
// frame, so the font file is read on the first frame the overlay draws (the menus), not at the
// first chat message, mid-match. The video thread, in the ImGui frame.
void Warm();
// The site's avatar colour for `name` (web/src/lib/site.ts avatarColor: FNV-1a of the lowercased
// name's UTF-16 units over seven colours; the same for every ASCII and Latin-1 name).
ImU32 AvatarColour(std::string_view name);
// `text`'s width at `px` pixels in the cards' face (Roboto, or ImGui's own without it).
float TextWidth(std::string_view text, float px);
// `text` broken into lines no wider than `width` at `px` pixels in the cards' face: at spaces, and
// through a word wider than a line.
std::vector<std::string> Wrap(std::string_view text, float px, float width);
// A chat message as a card, top-left at `pos`, at most `width` wide: the sender's avatar (their
// initial on their colour), their name, the message wrapped under it, YouGame's mark in the corner.
// `unit` is the overlay's (1% of the picture's height). Returns the card.
Kit::Box ChatCard(ImDrawList* dl, ImVec2 pos, float unit, std::string_view name,
                  std::string_view text, float width, float alpha = 1);
// A notice (someone joined or left) as a one-line card with YouGame's mark in the avatar's place.
Kit::Box NoticeCard(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float width,
                    float alpha = 1);
// YouGame's mark, `size` pixels square, top-left at `pos` (web/src/app/icon.svg's shapes).
void Logo(ImDrawList* dl, ImVec2 pos, float size, float alpha = 1);

// For the YouGame button and its lines (YgOrb.h), in the same look:
// The round mark the page's button shows over a game (PlayOrb.tsx RoundLogo: the logo's controller
// on a red disc), `size` pixels across, top-left at `pos`.
void RoundLogo(ImDrawList* dl, ImVec2 pos, float size, float alpha = 1);
// A line of text in the cards' face, top-left at `pos`, `px` pixels high.
void Text(ImDrawList* dl, ImVec2 pos, float px, ImU32 colour, std::string_view text);
// A card's slab over [min, max].
void CardSlab(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, float alpha = 1);
// The cards' colours: the main text, the secondary text, the hairline edge.
ImU32 Ink(float alpha = 1);
ImU32 Ink2(float alpha = 1);
ImU32 Edge(float alpha = 1);
// An avatar: `name`'s initial on its colour, a disc of radius `r` round `centre`.
void Avatar(ImDrawList* dl, ImVec2 centre, float r, std::string_view name, float alpha = 1);
// The same from a colour and an initial worked out before (AvatarColour, InitialOf), for a caller
// that draws the same person every frame and keeps them (Avatar converts the name each time).
std::string InitialOf(std::string_view name);
void AvatarOf(ImDrawList* dl, ImVec2 centre, float r, ImU32 colour, std::string_view initial,
              float alpha = 1);
}  // namespace Orca::UX::Yg
