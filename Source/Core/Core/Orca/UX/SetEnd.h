// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"

struct ImDrawList;

// The widgets' tones (Widgets.h, which needs ImGui's headers: this one doesn't, for the app's
// command loop).
namespace Orca::UX::Widgets
{
enum class Tone : int;
}

// How a matchmade set ends, on screen (ORCA.md "Set end"; Luis, 2026-10-04: "after winning or
// losing a set, we should get a victory or loss overlay that shows the results and elo change ...
// when someone leaves, I'm just kind of stuck in the match confused about what's going to happen"):
//
// - **VICTORY / DEFEAT.** When a ranked set ends (the match block's set is over on the results
//   screen, or the room's verdict on the set), a panel: the title, the set's score and the
//   opponent, and the player's rating counting from the old number to the new one with the change
//   (+16 green, –16 red) popping in as it lands. The new rating is the room's (its verdict carries
//   the site's ratings) or the page's second `queue rating <n>`; "Rating updating…" until then, no
//   rating after RatingCount::WAIT_MS. Casual: a smaller "YOU WON" / "YOU LOST" with the sitting's
//   score.
// - **Set over** (YouGame docs/ORCA_ONLINE_UX.md "3B set over"): the room stays open after the
//   verdict, so both players stay on the results screen together (and can talk on the page) until
//   each leaves: their results screen goes on, or they hold Z ("Hold Z to leave", with the hold's
//   progress). Nothing is a forfeit any more: the opponent leaving is "<name> left", and with them
//   gone Start leaves too.
// - **The opponent disconnected.** Mid-set, a centred notice with the truth of the wait: "<name>
//   disconnected · They forfeit in 0:09". The room's leave forfeits the set LEAVE_FORFEIT_MS after
//   it (rooms/src/game-lobby.ts GRACE) unless they come back; a stall with no leave ends when the
//   session gives up after STALL_FORFEIT_MS (OnlineMatch's PEER_SILENCE_LIMIT) and claims the set,
//   which the room lets stand at once (it heard nothing from them for STALL_SILENCE_MS, 9 s, by
//   then). The notice goes if they come back first; when the forfeit lands, "<name> forfeited · You
//   win the set" for FORFEITED_MS, then VICTORY. Casual: "<name> left · Searching again".
// - **This player left** a ranked set (the app's leave, or no pick in time): "You left · This set
//   counts as a loss", over its own character select.
//
// Local display only: nothing here reads or writes emulated memory, and nothing it decides goes
// back into the game. Its inputs are the frame hook's first runs (the block), the room thread
// (verdicts, leaves, set over), the app's lines and the session (its stall, the leave hint), any
// thread; the overlay draws At().
namespace Orca::UX::SetEnd
{
// ---- The rating's count (pure: the tests drive it with a clock) ----
class RatingCount
{
public:
  // "Rating updating…" this long at most, then no rating at all.
  static constexpr double WAIT_MS = 10000;
  // The count from the old number to the new, eased out.
  static constexpr double COUNT_MS = 1000;
  // The pop as it lands: the number and the change swell and settle.
  static constexpr double POP_MS = 450;
  static constexpr float POP_SCALE = 0.28f;

  enum class Phase
  {
    None,      // not started
    Updating,  // the set is over, the new rating isn't in: "Rating updating…"
    Counting,  // old -> new
    Landed,    // the new number (the pop, then still)
    Gone,      // nothing came in WAIT_MS: no rating shown
  };
  struct View
  {
    Phase phase = Phase::None;
    int shown = -1;  // the number to show (-1: none)
    // The change once the new number is in, with a number before it (has_delta); 0 is "±0".
    bool has_delta = false;
    int delta = 0;
    float pop = 1;        // the number's scale
    float badge_pop = 1;  // the change's scale (0 before it shows)
    bool operator==(const View&) const = default;
  };

  // The set ended at `now`; `before` the rating going into it (-1: unknown).
  void Start(int before, double now);
  // The new rating. `before` (when >= 0) replaces the number going in (the room's own). The same
  // number again changes nothing; another one counts on from where the count stands.
  void Arrive(int after, double now, int before = -1);
  bool Started() const { return m_started; }
  bool Arrived() const { return m_after >= 0; }
  int Before() const { return m_before; }
  int After() const { return m_after; }
  // When the count lands, or landed (-1 before a new number came).
  double LandsAt() const;
  View At(double now) const;

private:
  bool m_started = false;
  double m_start = 0;
  int m_before = -1;
  int m_from = -1;  // where the count starts: before, or where it stood when the number changed
  int m_after = -1;
  double m_arrived = -1;
};

// Whole seconds left before `deadline_ms` at `now_ms`, rounded up ("0:09" from 9 s down to just
// over 8 s), 0 at or past it.
int SecondsLeft(double deadline_ms, double now_ms);

enum class Outcome
{
  Won,
  Lost,
  Draw,
  Void,
};

// What the overlay draws this frame.
struct Notice
{
  std::string title;  // "bo disconnected", "bo forfeited", "You left"
  std::string line;   // "They forfeit in 0:09", "You win the set", "This set counts as a loss"
  int seconds = -1;   // the countdown's seconds left (its ring); -1: no countdown
  float fraction = 0;  // of the countdown's whole wait, left
  Widgets::Tone tone{};  // Neutral
  float alpha = 1;
  float scale = 1;  // its entrance
  bool operator==(const Notice&) const = default;
};
struct Panel
{
  bool ranked = true;
  Outcome out = Outcome::Won;
  std::string title;   // VICTORY, DEFEAT; casual YOU WON, YOU LOST, DRAW
  std::string score;   // "2–1", the player's games first
  std::string detail;  // "vs bo", "vs bo · by forfeit"
  bool show_rating = false;
  RatingCount::View rating;
  float alpha = 1;
  float scale = 1;  // its entrance
  bool operator==(const Panel&) const = default;
};
// Set over, on the results screen: how this player leaves ("Hold Z to leave"), and the hold.
struct Hint
{
  std::string text;
  float progress = 0;  // 0..1 of the hold (0: not held)
  bool operator==(const Hint&) const = default;
};
struct View
{
  std::optional<Notice> notice;
  std::optional<Panel> panel;
  std::optional<Hint> hint;
  // The notice says more than the overlay's "Waiting for <name>…": that line goes meanwhile.
  bool hides_waiting = false;
  bool Empty() const { return !notice && !panel && !hint; }
};

class Model
{
public:
  // The real timing end to end (see the namespace's comment).
  static constexpr double LEAVE_FORFEIT_MS = 15000;  // the room's GRACE after a leave
  static constexpr double STALL_FORFEIT_MS = 10000;  // OnlineMatch's PEER_SILENCE_LIMIT
  static constexpr double STALL_NOTICE_MS = 1500;    // a stall shorter than this isn't one to show
  // A countdown with no verdict this long past its end goes (OnlineMatch's VERDICT_WAIT is 30 s).
  static constexpr double NO_VERDICT_MS = 20000;
  static constexpr double FORFEITED_MS = 2500;  // "<name> forfeited · You win the set"
  static constexpr double PANEL_MS = 7000;      // the set's panel at least this long
  static constexpr double PANEL_AFTER_LAND_MS = 3000;  // and this long after the rating lands
  static constexpr double PANEL_MAX_MS = 15000;
  static constexpr double CASUAL_MS = 6000;    // a casual game's panel
  static constexpr double NOTICE_MS = 4000;    // "You left", "<name> left · Searching again"
  static constexpr double ENTER_MS = 220;
  static constexpr double FADE_MS = 400;

  // The room's match began (`start`): a ranked set, or a casual game, against `opponent`. Whatever
  // showed of the last one goes.
  void MatchBegan(bool ranked, std::string opponent, double now);
  // The match block, on a first run on the results screen: the ranked set is over. `key` names the
  // set (its last game's first frame), so a set opens its panel once; `rating_before` this
  // player's rating going in (Queue::OwnRating, -1 none).
  void BlockSetOver(u32 key, bool won, int mine, int theirs, int rating_before, double now);
  // The room's verdict on a ranked set: the room's tally, how (a forfeit or the games), and the
  // ratings it carries (-1: none). `rating_before` as above, for a panel that opens now.
  void SetVerdict(Outcome out, bool forfeit, int mine, int theirs, int before, int after,
                  int rating_before, double now);
  // A casual game's verdict, and the sitting's score with this opponent so far.
  void CasualGame(Outcome out, int mine, int theirs, double now);
  // The page's `queue rating <n>`: after a set, the new rating (the site rated the set).
  void QueueRating(int rating, double now);
  // Mid-set, the room says the opponent left: the forfeit is LEAVE_FORFEIT_MS away unless they
  // come back (OpponentBack).
  void OpponentLeft(double now);
  void OpponentBack(double now);
  // While the session waits on the opponent's inputs (OnlineMatch's stall loop): for how long.
  void Stall(int stalled_ms, double now);
  // That wait is over: `resumed` their inputs came (they are back), else the session gave up or
  // stopped.
  void StallOver(bool resumed, double now);
  // The session gave up waiting on them and unplugged them: the countdown stays for the verdict.
  void OpponentDropped(double now);
  // Casual: the opponent left the room, and the page searches again.
  void CasualOpponentLeft(double now);
  // This player left a ranked set: the app's leave, or no character picked in time (`no_show`).
  void SelfLeft(bool no_show, double now);
  // No verdict will come (the room ended, or the wait for it did): the countdown goes.
  void NoVerdict(double now);
  // The room's `set-over`: it stays open after the set's verdict until the players leave it.
  // Nothing that happens to the opponent from now on is a forfeit.
  void SetOver(double now);
  // Set over, the opponent left the room: "<name> left" (the leave hint says the way out).
  void OpponentLeftAfterSet(double now);
  // Set over, while this player is on the results screen (OnlineMatch, each boundary): `shown`,
  // the way out ("Hold Z to leave"; with the opponent gone, Start as well) and how far Z has been
  // held (0..1); `shown` false takes it away (left, or off the results screen).
  void LeaveHint(bool shown, bool opponent_gone, float progress);
  // Set over, this player's game is leaving the room: the opponent's going after this is no news
  // ("<name> left" would show over the player's own character select), and one already shown goes.
  void LeavingSetRoom(double now);
  bool InSetOver() const;

  View At(double now) const;
  // The opponent's name as the room gave it ("" none).
  std::string Opponent() const;

private:
  // Everything back to how it starts (under m_lock).
  void ResetLocked();
  void OpenPanel(bool ranked, Outcome out, int mine, int theirs, bool forfeit, int rating_before,
                 double now);
  std::optional<Notice> NoticeAt(double now, bool* hides_waiting) const;
  std::optional<Panel> PanelAt(double now) const;
  std::string Name() const;

  mutable std::mutex m_lock;
  std::string m_opponent;
  bool m_ranked_live = false;  // a ranked set is being played (the room's match, no verdict yet)
  bool m_casual_live = false;

  // The opponent gone mid-set: since when, and why.
  std::optional<double> m_left_at;   // the room's leave
  std::optional<double> m_stall_at;  // a stall's start (STALL_NOTICE_MS long or more)
  bool m_dropped = false;            // the session unplugged them
  std::optional<double> m_forfeited_at;  // the forfeit verdict came: "<name> forfeited"

  // The panel.
  bool m_panel = false;
  bool m_panel_ranked = true;
  Outcome m_out = Outcome::Won;
  int m_mine = 0, m_theirs = 0;
  bool m_by_forfeit = false;
  double m_panel_at = -1;  // when it shows (after "forfeited", FORFEITED_MS later)
  u32 m_block_key = 0;
  bool m_block_key_set = false;
  RatingCount m_rating;
  bool m_rating_from_room = false;  // the room's verdict carried it: the page's line is no news

  // A short notice of its own ("You left", "<name> left").
  std::string m_note_title, m_note_line;
  Widgets::Tone m_note_tone{};
  double m_note_at = -1;
  bool m_note_after_set = false;  // the note is set over's "<name> left"

  // Set over: the room stays open after the set; the leave hint while on the results screen.
  bool m_set_over = false;
  bool m_set_over_leaving = false;
  bool m_hint = false;
  bool m_hint_alone = false;
  float m_hint_progress = 0;
};

// The process's.
Model& Current();
// The clock every caller gives the model: the host's steady clock, in milliseconds.
double NowMs();

// Draws `view` in the picture (its top left corner at `pic_x`, `pic_y`, `pic_w` by `pic_h`), centred
// across it: the panel from 15% of its height down (casual's 20%), the notice under the panel, or
// from 30% of the height without one, and the leave hint near the bottom.
void Draw(ImDrawList* dl, float unit, float pic_x, float pic_y, float pic_w, float pic_h,
          const View& view);

// Tests only (ORCA_UX_SETEND_DEMO=victory|defeat|countdown|casual|left|stay, with test knobs allowed):
// the model plays that screen through, again every 16 s, from the first overlay frame, for design
// checks and screenshots. With ORCA_UX_SETEND_DEMO_AT=<frame> in a harness run it starts at that
// frame of the game and runs on the game's frames (60 a second) in place of the host's clock, so a
// screenshot at a given frame always shows the same moment. The overlay calls it every frame and
// views the model at the time it returns (`now` itself without the variables).
double DemoFrame(double now);
}  // namespace Orca::UX::SetEnd
