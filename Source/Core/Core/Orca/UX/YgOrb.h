// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
// Host-only drawing: nothing here reads or writes emulated state. This header is the state and the
// app's lines (no ImGui: the platform layer, DolphinNoGUI, includes it); YgOrbDraw.h draws it.
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

// A line's strength at `now_ms` when it came at `at_ms`: 1, fading over its last FADE_MS, 0 once
// its dwell is over.
float LineAlpha(const Notice& notice, double at_ms, double now_ms);

// What the button and its lines show. Pure state, so the tests drive it with a clock. Drawn every
// frame, so what a frame reads never allocates: At() is the button's state and whether a line is
// showing, and the lines themselves are copied out (Lines) only when they change.
class Model
{
public:
  static constexpr double FADE_MS = 400;
  static constexpr std::size_t KEEP = 3;
  // One ring of a blink; a blink is three (the page's orb-ring, 0.7 s × 3).
  static constexpr double RING_MS = 700;
  static constexpr int RINGS = 3;

  struct Entry
  {
    double at = 0;  // when it came
    Notice notice;
  };
  struct View
  {
    bool on = false;
    Place place;
    // A blink's ring, 0..1 through its growth; negative: none.
    float ring = -1;
    // The lines' generation (a new one with each line), and whether any is showing now.
    std::uint64_t generation = 0;
    bool showing = false;
  };

  void Apply(const Command& command, double now_ms);
  void Add(Notice notice, double now_ms);
  View At(double now_ms) const;
  // The newest KEEP lines, oldest first, into `out` when `generation` is not the current one (a
  // copy once per change). Returns the current generation.
  std::uint64_t Lines(std::uint64_t generation, std::vector<Entry>* out) const;
  // A press at (x, y), the view's pixels, on the button's disc.
  bool Hit(float x, float y) const;

private:
  mutable std::mutex m_lock;
  bool m_on = false;
  Place m_place;
  double m_blink_at = -1;
  std::deque<Entry> m_lines;
  std::uint64_t m_generation = 0;
};

// The process's button (the stdin thread applies, the video thread draws), on the overlay's clock
// (steady time in ms, Overlay.cpp's NowMs).
Model& Current();
double NowMs();
// The app's `orb` and `notice` lines, now. Any thread.
void Apply(const Command& command);
void Show(Notice notice);
}  // namespace Orca::UX::YgOrb
