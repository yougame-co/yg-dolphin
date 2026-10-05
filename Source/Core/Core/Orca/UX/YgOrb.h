// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

#include "Core/Orca/UX/Kit.h"

// The YouGame button over a game Orca fills the screen with (embed cap `yougame`; YouGame's
// docs/PLAY_OVERLAY.md "Desktop app and Orca"). The page's own button sits at the game's top left,
// under Orca's window, where nothing on the page can be seen; so Orca draws it in its place, and
// the overlay's lines beside it in YouGame's cards (YgCard.h):
//   orb <x> <y> <size> <lit> <away> <badge>
//       the button: a disc `size` pixels across, its top left at (x, y) in the view's pixels (the
//       page's own button's rectangle); lit 1 at full strength (0: half, at rest), away 1 with the
//       amber dot of a held seat, badge 0..99 unread (shown "9+" past 9).
//   orb blink    a white ring grows out of it three times (something from outside the lobby).
//   orb off      not drawn (the play menu is open, or the game no longer fills the screen).
//   notice <kind> <name> <text> [<key> <verb>]
//       a line beside the button for as long as the page's own would show (DwellMs): kind
//       join|leave|away|back|voice|call|dm|invite, the other fields encodeURIComponent as in `chat`
//       (Chat.h); key and verb the action's hint ("Shift%2BTab" "Reply"), drawn grey.
// Clicks: on macOS they reach the page's own button under it (Orca's window takes no mouse); on
// Windows a press on the disc prints `orca orb` instead of `orca click`.
// Host-only drawing: nothing here reads or writes emulated state.
namespace Orca::UX::YgOrb
{
enum class Kind
{
  Join,
  Leave,
  Away,
  Back,
  Voice,
  Call,
  Dm,
  Invite,
};

struct Place
{
  float x = 0;
  float y = 0;
  float size = 0;
  bool lit = false;
  bool away = false;
  int badge = 0;
};

struct Command
{
  enum class Type
  {
    Off,
    Blink,
    Place,
  };
  Type type = Type::Off;
  Place place;
};
// The whole stdin line "orb …"; nullopt when malformed (the caller answers "unsupported orb").
std::optional<Command> ParseOrb(std::string_view line);

struct Notice
{
  Kind kind = Kind::Join;
  std::string name;
  std::string text;
  std::string key;   // empty: no hint
  std::string verb;
};
// The whole stdin line "notice …"; nullopt when malformed (the caller answers "unsupported
// notice"). The fields are a player's words: never printed or logged.
std::optional<Notice> ParseNotice(std::string_view line);
// The hint's fields, decoded: at most this many code points each.
inline constexpr std::size_t HINT_POINTS = 24;

// How long a notice stays, as the page's own line does (PlayOrb.tsx DWELL): ms.
double DwellMs(const Notice& notice);

// What the button and its lines show at a moment. Pure state, so the tests drive it with a clock.
class Model
{
public:
  static constexpr double FADE_MS = 400;
  static constexpr std::size_t KEEP = 3;
  // One ring of a blink; a blink is three (the page's orb-ring, 0.7 s × 3).
  static constexpr double RING_MS = 700;
  static constexpr int RINGS = 3;

  struct Line
  {
    Notice notice;
    float alpha = 1;
  };
  struct View
  {
    bool on = false;
    Place place;
    // A blink's ring, 0..1 through its growth; negative: none.
    float ring = -1;
    std::vector<Line> lines;  // oldest first, the newest KEEP
  };

  void Apply(const Command& command, double now_ms);
  void Add(Notice notice, double now_ms);
  View At(double now_ms) const;
  // A press at (x, y), the view's pixels, on the button's disc.
  bool Hit(float x, float y) const;

private:
  mutable std::mutex m_lock;
  bool m_on = false;
  Place m_place;
  double m_blink_at = -1;
  std::deque<std::pair<double, Notice>> m_lines;  // (when it came, the notice)
};

// The process's button (the stdin thread applies, the video thread draws), on the overlay's clock
// (steady time in ms, Overlay.cpp's NowMs).
Model& Current();
double NowMs();
// The app's `orb` and `notice` lines, now. Any thread.
void Apply(const Command& command);
void Show(Notice notice);
// The button's box, its dot and badge included; an empty box (min == max) when off.
Kit::Box ButtonBox(const Model::View& view);
// The button at its place with its ring, dot and badge (the video thread, in the overlay's ImGui
// frame).
void DrawButton(ImDrawList* dl, const Model::View& view);
// The lines as a column, each card's top left at `pos.x` and the first's at `pos.y`, at most
// `width` wide and none starting past `bottom`. Returns the y under the last.
float DrawLines(ImDrawList* dl, const Model::View& view, ImVec2 pos, float unit, float width,
                float bottom);
}  // namespace Orca::UX::YgOrb
