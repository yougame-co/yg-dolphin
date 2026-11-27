// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/BrawlOnlineEntry.h"

#include <algorithm>
#include <atomic>
#include <vector>

#include <imgui.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/BrawlOnlineSignIn.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/Overlay.h"

namespace Orca::UX::BrawlOnlineEntry
{
namespace
{
// Sizes, in overlay units: the sign-in screen's.
constexpr float PANEL_MAX_WIDTH = 76;
constexpr float PAD = 3;
constexpr float TITLE_SIZE = 5;
constexpr float TEXT_SIZE = 2.6f;
constexpr float GAP = 1.4f;

// 0: none; otherwise the row's OnlinePick + 1.
std::atomic<int> s_row{0};

// Lays the panel out from `top`; draws when `dl` is set. Returns its height.
float Layout(ImDrawList* dl, std::string_view title_text, float unit, float centre_x, float top,
             float width)
{
  const float pad = PAD * unit;
  const float inner = width - 2 * pad;
  float y = top + pad;
  float title_size = TITLE_SIZE;
  if (const float wide = Kit::Measure(Kit::TitleLook(unit, TITLE_SIZE), title_text).x; wide > inner)
    title_size *= inner / wide;
  if (dl)
    Kit::Title(dl, ImVec2(centre_x, y), unit, title_text, title_size, Kit::Align::Centre);
  y += Kit::Measure(Kit::TitleLook(unit, title_size), title_text).y + GAP * unit;

  const Kit::TextLook text = Kit::LabelLook(unit, TEXT_SIZE);
  const float line = Kit::Measure(text, "Ag").y;
  for (const std::string_view paragraph : {BrawlOnlineSignIn::Note(), BACK_HINT})
  {
    for (const std::string& part : Kit::Wrap(text, paragraph, inner))
    {
      if (dl)
      {
        Kit::Label(dl, ImVec2(centre_x, y), unit, part, TEXT_SIZE, Kit::Align::Centre,
                   IM_COL32(235, 235, 240, 255));
      }
      y += line;
    }
    y += GAP * unit;
  }
  return y - GAP * unit + pad - top;
}
}  // namespace

std::string PlayerId()
{
  std::string id = Orca::GetEnv("ORCA_YOUGAME_PLAYER");
  return id.empty() ? std::string(LOCAL_PLAYER) : id;
}

void PageOpened()
{
  if (BrawlOnlineSignIn::IsOpen())
    return;
  const std::string kept = BrawlOnlineSignIn::Open(PlayerId(), "");
  NOTICE_LOG_FMT(ROLLBACK, "Brawl Online page: {}",
                 !kept.empty()                   ? "link kept on this PC" :
                 BrawlOnlineSignIn::IsOpen()     ? "sign-in screen" :
                 BrawlOnlineSignIn::OnlineActive() ? "no sign-in screen while playing online" :
                                                     "sign-in screen did not open");
}

void SetRow(std::optional<OnlinePick> row)
{
  const bool is_row = row == OnlinePick::BrawlOnlineCasual || row == OnlinePick::BrawlOnlineFriends;
  s_row.store(is_row ? static_cast<int>(*row) + 1 : 0, std::memory_order_relaxed);
}

std::optional<OnlinePick> Row()
{
  const int v = s_row.load(std::memory_order_relaxed);
  if (v == 0)
    return std::nullopt;
  return static_cast<OnlinePick>(v - 1);
}

std::string_view RowTitle(OnlinePick row)
{
  switch (row)
  {
  case OnlinePick::BrawlOnlineCasual:
    return "Brawl Online: Casual";
  case OnlinePick::BrawlOnlineFriends:
    return "Brawl Online: With Friends";
  default:
    return {};
  }
}

void Draw()
{
  const std::optional<OnlinePick> row = Row();
  if (!row || BrawlOnlineSignIn::IsOpen())
    return;
  const std::string_view title = RowTitle(*row);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float unit = OverlayUnit(display.y);
  if (unit <= 0 || !(display.x >= 1) || title.empty())
    return;
  const float width = std::min(display.x - 4 * unit, PANEL_MAX_WIDTH * unit);
  const float centre_x = display.x / 2;
  const float height = Layout(nullptr, title, unit, centre_x, 0, width);
  const float top = std::max(0.0f, (display.y - height) / 2);
  // Drawn on the background list, under every overlay window: nothing here takes input.
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 150));
  Kit::Plate(dl, ImVec2(centre_x - width / 2, top), ImVec2(centre_x + width / 2, top + height),
             unit, Kit::Tone::Panel);
  Layout(dl, title, unit, centre_x, top, width);
}
}  // namespace Orca::UX::BrawlOnlineEntry
