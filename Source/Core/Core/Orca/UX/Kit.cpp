// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Kit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/UX/GameFont.h"
#include "Core/Orca/UX/Overlay.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"

namespace Orca::UX::Kit
{
namespace
{
constexpr int FACES = 2;
// Where each face is on the disc (Brawl's `system/font/`, which Project+ loads unchanged).
constexpr std::array<const char*, FACES> FONT_FILES = {
    "system/font/font_latin1.arc",  // Body: the menus' rounded sans, LZ77
    "system/font/font_hira.brfnt",  // Heavy: the heavy display face
};
constexpr const char* GAME_ID = "RSBE01";
constexpr int ATLAS_LEVELS = 4;

std::mutex s_lock;
std::string s_disc;
bool s_started = false;
std::thread s_reader;
std::atomic<bool> s_read{false};
std::array<std::optional<GameFont::Atlas>, FACES> s_read_atlases;  // under s_lock

// The video thread's: each face's atlas on the GPU. A texture made by a backend that is gone is
// never freed (its device went with it); a new backend gets new ones.
struct GpuFace
{
  const AbstractGfx* gfx = nullptr;
  bool tried = false;
  std::unique_ptr<AbstractTexture> texture;
  GameFont::Atlas atlas;  // its glyphs and metrics (no pixels)
};
std::array<GpuFace, FACES> s_gpu;
std::atomic<int> s_faces_ready{0};  // bit per face

void ReadFonts(std::string path)
{
  std::array<std::optional<GameFont::Atlas>, FACES> atlases;
  std::string error;
  std::vector<std::optional<GameFont::Rfnt>> fonts =
      GameFont::ReadFromDisc(path, FONT_FILES, GAME_ID, &error);
  if (!error.empty())
    WARN_LOG_FMT(ROLLBACK, "Orca kit: the game's fonts: {}", error);
  const std::vector<char32_t> codes = GameFont::OverlayCodePoints();
  for (int face = 0; face < FACES; ++face)
  {
    if (!fonts[static_cast<std::size_t>(face)])
      continue;
    atlases[face] = GameFont::BuildAtlas(*fonts[static_cast<std::size_t>(face)], codes, 4,
                                         ATLAS_LEVELS);
    NOTICE_LOG_FMT(ROLLBACK, "Orca kit: {} read, {} glyphs, {}x{}", FONT_FILES[face],
                   atlases[face]->glyphs.size(), atlases[face]->width, atlases[face]->height);
  }
  {
    std::lock_guard lk(s_lock);
    s_read_atlases = std::move(atlases);
  }
  s_read = true;
}

struct FaceRef
{
  const GameFont::Atlas* atlas;
  ImTextureRef texture;
};

std::array<std::pair<const GameFont::Atlas*, ImTextureID>, FACES> s_test_faces{};

std::optional<FaceRef> GameFace(Face face)
{
  if (const auto& [atlas, id] = s_test_faces[static_cast<int>(face)]; atlas)
    return FaceRef{atlas, ImTextureRef(id)};
  const GpuFace& g = s_gpu[static_cast<int>(face)];
  if (!g.texture || !g_gfx || g.gfx != g_gfx.get())
    return std::nullopt;
  return FaceRef{&g.atlas, static_cast<ImTextureRef>(*g.texture)};
}

// The ImGui pass samples its textures with a point sampler; the game's glyphs are drawn smaller
// than they are stored, so they take a trilinear one, and the pass gets its own back after them.
void LinearSampler(const ImDrawList*, const ImDrawCmd*)
{
  if (g_gfx)
    g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());
}
void PointSampler(const ImDrawList*, const ImDrawCmd*)
{
  if (g_gfx)
    g_gfx->SetSamplerState(0, RenderState::GetPointSamplerState());
}

ImU32 Rgb(int r, int g, int b, float a = 1)
{
  return IM_COL32(r, g, b, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)));
}

// `c` with its alpha times `alpha`.
ImU32 Fade(ImU32 c, float alpha)
{
  const float a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
  return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::lround(a)) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t)
{
  t = std::clamp(t, 0.0f, 1.0f);
  ImU32 out = 0;
  for (int shift = 0; shift < 32; shift += 8)
  {
    const float x = static_cast<float>((a >> shift) & 0xFF);
    const float y = static_cast<float>((b >> shift) & 0xFF);
    out |= static_cast<ImU32>(std::lround(x + (y - x) * t)) << shift;
  }
  return out;
}

// The vertices from `first` on, recoloured top to bottom (by their y from `y0` to `y1`), each
// keeping its own alpha as a factor: ImGui's anti-aliased fringe fades out as it did in white.
void Recolour(ImDrawList* dl, int first, float y0, float y1, ImU32 top, ImU32 bottom)
{
  const float span = std::max(1e-3f, y1 - y0);
  for (int i = first; i < dl->VtxBuffer.Size; ++i)
  {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float own = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    v.col = Fade(Mix(top, bottom, (v.pos.y - y0) / span), own);
  }
}

// A tone as Brawl's own buttons paint it (the character select's "Friend Registration Requested!",
// "Unable to register.", "Awaiting reply...", the ITEM and STAGE tags): a light rim lit from above,
// a dark line inside it, the body's gradient, and the text's ink.
struct Paint
{
  ImU32 top, bottom;          // the body
  ImU32 rim_top, rim_bottom;  // the rim
  ImU32 line;                 // between rim and body
  ImU32 ink, ink_outline;     // text on it; an outline of 0 alpha: none
};

Paint PaintOf(Tone tone)
{
  const ImU32 rim_top = Rgb(255, 255, 255), rim_bottom = Rgb(186, 190, 202);
  switch (tone)
  {
  case Tone::Gold:
    return {Rgb(255, 236, 96), Rgb(244, 164, 0), rim_top, rim_bottom, Rgb(44, 26, 0),
            Rgb(24, 16, 0), Rgb(0, 0, 0, 0)};
  case Tone::Yellow:
    return {Rgb(255, 226, 70), Rgb(226, 150, 0), rim_top, rim_bottom, Rgb(44, 26, 0),
            Rgb(24, 16, 0), Rgb(0, 0, 0, 0)};
  case Tone::Grey:
    return {Rgb(240, 240, 244), Rgb(196, 198, 206), rim_top, Rgb(206, 208, 216),
            Rgb(150, 152, 160), Rgb(138, 140, 150), Rgb(255, 255, 255, 0.9f)};
  case Tone::Red:
    return {Rgb(255, 112, 98), Rgb(204, 18, 22), rim_top, rim_bottom, Rgb(36, 0, 0),
            Rgb(255, 255, 255), Rgb(90, 0, 0)};
  case Tone::Blue:
    return {Rgb(104, 176, 255), Rgb(18, 72, 214), rim_top, rim_bottom, Rgb(0, 8, 40),
            Rgb(255, 255, 255), Rgb(0, 20, 90)};
  case Tone::Green:
    return {Rgb(118, 232, 112), Rgb(22, 146, 40), rim_top, rim_bottom, Rgb(0, 30, 4),
            Rgb(255, 255, 255), Rgb(0, 60, 10)};
  case Tone::Navy:
  default:
    return {Rgb(44, 70, 140, 0.95f), Rgb(12, 22, 62, 0.95f), Rgb(236, 240, 250),
            Rgb(150, 162, 186), Rgb(4, 8, 22), Rgb(255, 255, 255), Rgb(4, 8, 22)};
  }
}

// Text in ImGui's font where the game's isn't there (yet), drawn the same way.
ImVec2 FallbackMeasure(const TextLook& look, std::string_view text)
{
  return ImGui::GetFont()->CalcTextSizeA(std::max(1.0f, look.px), FLT_MAX, 0, text.data(),
                                         text.data() + text.size());
}

// Calls `emit(glyph, x)` for every glyph of `text` in the atlas (x in font pixels from the start,
// before scaling), substituting what the face lacks; returns the advance of the whole line.
// The word gap, at least this much of the line: the heavy face's own space is thin.
float MinSpace(Face face)
{
  return face == Face::Heavy ? 0.42f : 0.0f;
}

template <typename Emit>
float LayOut(const GameFont::Atlas& atlas, float tracking_font_px, float min_space,
             std::string_view text, Emit&& emit)
{
  float x = 0;
  const auto put = [&](auto&& self, char32_t code, int depth) -> void {
    const auto it = atlas.glyphs.find(code);
    if (it != atlas.glyphs.end())
    {
      emit(it->second, x);
      const float advance =
          code == U' ' ? std::max(it->second.advance, atlas.cell_height * min_space) :
                         it->second.advance;
      x += advance + tracking_font_px;
      return;
    }
    const std::u32string sub = depth < 3 ? GameFont::Substitute(code) : std::u32string();
    if (sub.empty())
    {
      x += atlas.space + tracking_font_px;
      return;
    }
    for (const char32_t c : sub)
      self(self, c, depth + 1);
  };
  for (std::size_t i = 0; i < text.size();)
  {
    const char32_t code = GameFont::NextCodePoint(text, &i);
    if (code == U'\n' || code == U'\r')
      continue;
    put(put, code, 0);
  }
  return std::max(0.0f, x - tracking_font_px);
}

// One pass of `text`'s glyphs at `pos`: a quad each, `top` to `bottom`.
void GlyphPass(ImDrawList* dl, const GameFont::Atlas& atlas, ImVec2 pos, float scale,
               const TextLook& look, std::string_view text, ImU32 top, ImU32 bottom)
{
  const float h = atlas.cell_height * scale;
  const float skew = look.italic * h;
  LayOut(atlas, look.tracking / scale, MinSpace(look.face), text,
         [&](const GameFont::Atlas::Glyph& g, float x) {
    if (g.width <= 0)
      return;
    const float x0 = pos.x + (x + g.left) * scale;
    const float x1 = x0 + g.width * scale;
    const float y0 = pos.y, y1 = pos.y + h;
    dl->PrimReserve(6, 4);
    const auto base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
    dl->PrimWriteVtx(ImVec2(x0 + skew, y0), ImVec2(g.u0, g.v0), top);
    dl->PrimWriteVtx(ImVec2(x1 + skew, y0), ImVec2(g.u1, g.v0), top);
    dl->PrimWriteVtx(ImVec2(x1, y1), ImVec2(g.u1, g.v1), bottom);
    dl->PrimWriteVtx(ImVec2(x0, y1), ImVec2(g.u0, g.v1), bottom);
  });
}

// The directions an outline is drawn in: 8 for a thin one, 12 past 2 pixels.
template <typename F>
void AroundOutline(float outline, F&& f)
{
  const int n = outline > 2.0f ? 12 : 8;
  for (int k = 0; k < n; ++k)
  {
    const float a = 2 * std::numbers::pi_v<float> * static_cast<float>(k) / static_cast<float>(n);
    f(ImVec2(std::cos(a) * outline, std::sin(a) * outline));
  }
}

ImVec2 Add(ImVec2 a, ImVec2 b)
{
  return ImVec2(a.x + b.x, a.y + b.y);
}

void FillRounded(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius, ImU32 top, ImU32 bottom,
                 ImDrawFlags flags = ImDrawFlags_None)
{
  if (max.x <= min.x || max.y <= min.y)
    return;
  const int first = dl->VtxBuffer.Size;
  dl->AddRectFilled(min, max, IM_COL32_WHITE, std::max(0.0f, radius), flags);
  Recolour(dl, first, min.y, max.y, top, bottom);
}

float Snap(float v)
{
  return std::floor(v + 0.5f);
}

// The last seconds' pulse: 1 as a second begins, easing to 0 by its middle.
float Pulse(double now_s)
{
  const float t = static_cast<float>(now_s - std::floor(now_s));
  const float k = std::max(0.0f, 1.0f - 2.0f * t);
  return k * k;
}
}  // namespace

Tone PortTone(int port)
{
  switch (port)
  {
  case 0:
    return Tone::Red;
  case 1:
    return Tone::Blue;
  case 2:
    return Tone::Yellow;
  default:
    return Tone::Green;
  }
}

TextLook LabelLook(float unit, float size)
{
  TextLook look;
  look.face = Face::Body;
  look.px = std::max(1.0f, size * unit);
  look.outline = std::max(1.0f, look.px * 0.08f);
  look.outline_colour = Rgb(10, 10, 16);
  look.shadow = ImVec2(0, std::max(1.0f, look.px * 0.06f));
  look.shadow_colour = IM_COL32(0, 0, 0, 110);
  return look;
}

TextLook TitleLook(float unit, float size)
{
  TextLook look;
  look.face = Face::Heavy;
  look.px = std::max(1.0f, size * unit);
  look.top = IM_COL32_WHITE;
  look.bottom = Rgb(212, 222, 244);
  look.outline = std::max(1.0f, look.px * 0.085f);
  look.outline_colour = Rgb(10, 10, 16);
  look.shadow = ImVec2(look.px * 0.03f, look.px * 0.07f);
  look.shadow_colour = IM_COL32(0, 0, 0, 120);
  look.italic = 0.1f;
  look.tracking = look.px * 0.01f;
  return look;
}

void SetGameDisc(std::string path)
{
  std::lock_guard lk(s_lock);
  s_disc = std::move(path);
}

void BeginFrame()
{
  {
    std::lock_guard lk(s_lock);
    if (!s_started && !s_disc.empty())
    {
      s_started = true;
      s_reader = std::thread(ReadFonts, s_disc);
    }
  }
  if (!s_read.load(std::memory_order_acquire) || !g_gfx)
    return;
  int ready = 0;
  for (int face = 0; face < FACES; ++face)
  {
    GpuFace& g = s_gpu[face];
    if (g.gfx != g_gfx.get())
    {
      (void)g.texture.release();  // the old backend's (see GpuFace)
      g.gfx = g_gfx.get();
      g.tried = false;
    }
    if (!g.tried)
    {
      g.tried = true;
      std::lock_guard lk(s_lock);
      const std::optional<GameFont::Atlas>& read = s_read_atlases[face];
      if (read && read->width > 0 && !read->levels.empty())
      {
        const u32 levels = static_cast<u32>(read->levels.size());
        const TextureConfig config(static_cast<u32>(read->width), static_cast<u32>(read->height),
                                   levels, 1, 1, AbstractTextureFormat::RGBA8, 0,
                                   AbstractTextureType::Texture_2DArray);
        g.texture = g_gfx->CreateTexture(config, "Orca kit font");
        if (g.texture)
        {
          for (u32 level = 0; level < levels; ++level)
          {
            const u32 w = std::max(1u, config.width >> level);
            const u32 h = std::max(1u, config.height >> level);
            g.texture->Load(level, w, h, w, read->levels[level].data(), read->levels[level].size());
          }
          g.atlas = *read;
          g.atlas.levels.clear();
        }
      }
    }
    if (g.texture)
      ready |= 1 << face;
  }
  s_faces_ready = ready;
}

void Shutdown()
{
  std::thread reader;
  {
    std::lock_guard lk(s_lock);
    reader = std::move(s_reader);
  }
  if (reader.joinable())
    reader.join();
}

bool HasGameFont(Face face)
{
  return (s_faces_ready.load(std::memory_order_relaxed) >> static_cast<int>(face)) & 1;
}

ImVec2 Measure(const TextLook& look, std::string_view text)
{
  if (const std::optional<FaceRef> face = GameFace(look.face))
  {
    const float scale = look.px / face->atlas->cell_height;
    const float w = LayOut(*face->atlas, look.tracking / scale, MinSpace(look.face), text,
                           [](const GameFont::Atlas::Glyph&, float) {});
    return ImVec2(w * scale + look.italic * look.px * 0.5f + 2 * look.outline, look.px);
  }
  const ImVec2 s = FallbackMeasure(look, text);
  return ImVec2(s.x + 2 * look.outline, s.y);
}

std::vector<std::string> Wrap(const TextLook& look, std::string_view text, float width)
{
  std::vector<std::string> lines;
  std::string line;
  std::size_t at = 0;
  while (at < text.size())
  {
    const std::size_t space = text.find(' ', at);
    const std::string_view word =
        text.substr(at, space == std::string_view::npos ? std::string_view::npos : space - at);
    at = space == std::string_view::npos ? text.size() : space + 1;
    if (word.empty())
      continue;
    std::string longer = line.empty() ? std::string(word) : line + " " + std::string(word);
    if (!line.empty() && Measure(look, longer).x > width)
    {
      lines.push_back(std::move(line));
      line = std::string(word);
    }
    else
    {
      line = std::move(longer);
    }
  }
  if (!line.empty())
    lines.push_back(std::move(line));
  return lines;
}

ImVec2 Text(ImDrawList* dl, ImVec2 pos, const TextLook& look, std::string_view text, float alpha,
            Align align)
{
  const ImVec2 size = Measure(look, text);
  if (text.empty() || alpha <= 0)
    return size;
  if (align == Align::Centre)
    pos.x -= size.x / 2;
  else if (align == Align::Right)
    pos.x -= size.x;
  pos = ImVec2(Snap(pos.x + look.outline), Snap(pos.y));
  // Passes drawn over each other add up: an outline (or a shadow) of n offset passes takes each at
  // the alpha whose n-fold stack is the one asked for, so it fades with the rest and a shadow's
  // edges come out soft.
  const int passes = look.outline > 2.0f ? 12 : 8;
  const auto per_pass = [passes](float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    return a >= 1.0f ? 1.0f : 1.0f - std::pow(1.0f - a, 1.0f / static_cast<float>(passes));
  };
  const float shadow_a = static_cast<float>((look.shadow_colour >> IM_COL32_A_SHIFT) & 0xFF) / 255;
  const float outline_a = static_cast<float>((look.outline_colour >> IM_COL32_A_SHIFT) & 0xFF) / 255;
  const ImU32 shadow = Fade(look.shadow_colour, look.outline > 0 ?
                                                    per_pass(shadow_a * alpha) / std::max(1e-3f, shadow_a) :
                                                    alpha);
  const ImU32 outline = Fade(look.outline_colour, per_pass(outline_a * alpha) / std::max(1e-3f, outline_a));
  const ImU32 top = Fade(look.top, alpha), bottom = Fade(look.bottom, alpha);
  const bool has_shadow = (look.shadow.x != 0 || look.shadow.y != 0) && shadow_a > 0;
  if (const std::optional<FaceRef> face = GameFace(look.face))
  {
    const float scale = look.px / face->atlas->cell_height;
    dl->AddCallback(&LinearSampler, nullptr);
    dl->PushTexture(face->texture);
    if (has_shadow)
    {
      const ImVec2 at = Add(pos, look.shadow);
      if (look.outline > 0)
      {
        AroundOutline(look.outline, [&](ImVec2 d) {
          GlyphPass(dl, *face->atlas, Add(at, d), scale, look, text, shadow, shadow);
        });
      }
      else
      {
        GlyphPass(dl, *face->atlas, at, scale, look, text, shadow, shadow);
      }
    }
    if (look.outline > 0)
    {
      AroundOutline(look.outline, [&](ImVec2 d) {
        GlyphPass(dl, *face->atlas, Add(pos, d), scale, look, text, outline, outline);
      });
    }
    GlyphPass(dl, *face->atlas, pos, scale, look, text, top, bottom);
    dl->PopTexture();
    dl->AddCallback(&PointSampler, nullptr);
    return size;
  }
  ImFont* font = ImGui::GetFont();
  const float px = std::max(1.0f, look.px);
  const char* b = text.data();
  const char* e = text.data() + text.size();
  if (has_shadow && look.outline > 0)
  {
    const ImVec2 at = Add(pos, look.shadow);
    AroundOutline(look.outline,
                  [&](ImVec2 d) { dl->AddText(font, px, Add(at, d), shadow, b, e); });
  }
  else if (has_shadow)
  {
    dl->AddText(font, px, Add(pos, look.shadow), shadow, b, e);
  }
  if (look.outline > 0)
  {
    AroundOutline(look.outline,
                  [&](ImVec2 d) { dl->AddText(font, px, Add(pos, d), outline, b, e); });
  }
  const int first = dl->VtxBuffer.Size;
  dl->AddText(font, px, pos, IM_COL32_WHITE, b, e);
  Recolour(dl, first, pos.y, pos.y + size.y, top, bottom);
  return size;
}

namespace
{
// Brawl's button over [min, max]: a soft shadow under it, the rim lit from above, the dark line
// inside the rim, the body's gradient and a gloss over its top half.
void Slab(ImDrawList* dl, ImVec2 min, ImVec2 max, float radius, float unit, const Paint& p,
          float alpha)
{
  const float h = max.y - min.y;
  const float rim = std::clamp(h * 0.11f, 1.0f, 0.55f * unit);
  const float line = std::clamp(h * 0.05f, 1.0f, 0.3f * unit);
  dl->AddRectFilled(ImVec2(min.x + 0.1f * unit, min.y + 0.35f * unit),
                    ImVec2(max.x + 0.35f * unit, max.y + 0.55f * unit), Rgb(0, 0, 0, 0.34f * alpha),
                    radius + 0.2f * unit);
  FillRounded(dl, min, max, radius, Fade(p.rim_top, alpha), Fade(p.rim_bottom, alpha));
  const ImVec2 lmin(min.x + rim, min.y + rim), lmax(max.x - rim, max.y - rim);
  FillRounded(dl, lmin, lmax, std::max(0.0f, radius - rim), Fade(p.line, alpha),
              Fade(p.line, alpha));
  const ImVec2 bmin(lmin.x + line, lmin.y + line), bmax(lmax.x - line, lmax.y - line);
  const float r = std::max(0.0f, radius - rim - line);
  FillRounded(dl, bmin, bmax, r, Fade(p.top, alpha), Fade(p.bottom, alpha));
  FillRounded(dl, bmin, ImVec2(bmax.x, bmin.y + (bmax.y - bmin.y) * 0.5f), r,
              Rgb(255, 255, 255, 0.3f * alpha), Rgb(255, 255, 255, 0.04f * alpha),
              ImDrawFlags_RoundCornersTop);
}

// The ink a tone's text takes: white outlined in the tone's dark (black on gold and yellow, grey
// outlined in white on grey).
TextLook Inked(TextLook look, const Paint& p)
{
  look.top = p.ink;
  look.bottom = Mix(p.ink, Rgb(0, 0, 0), (p.ink & 0xFFFFFF) == 0xFFFFFF ? 0.08f : 0.0f);
  look.outline_colour = p.ink_outline;
  if (((p.ink_outline >> IM_COL32_A_SHIFT) & 0xFF) == 0)
  {
    look.outline = 0;
    look.shadow = ImVec2(0, 0);
  }
  return look;
}
}  // namespace

void Plate(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, float alpha)
{
  if (!(max.x > min.x && max.y > min.y) || alpha <= 0)
    return;
  const float radius = std::clamp(std::min(max.y - min.y, max.x - min.x) * 0.3f, 0.5f * unit,
                                  2.2f * unit);
  Slab(dl, min, max, radius, unit, PaintOf(tone), alpha);
}

Box Banner(ImDrawList* dl, float left, float right, float top, float height, float unit, Tone tone,
           std::string_view title, std::string_view note, float alpha)
{
  const Box box{ImVec2(left, top), ImVec2(right, top + height)};
  if (!(right > left && height > 0) || alpha <= 0)
    return box;
  const Paint p = PaintOf(tone);
  const float cut = height * 0.42f;
  const float bottom = top + height;
  // A slanted band (its ends cut parallel), built like the buttons: shadow, rim, line, body.
  const auto band = [&](float grow_x, float grow_y, ImVec2 d) {
    const float k = cut / height;
    return std::array<ImVec2, 4>{
        ImVec2(left + cut - grow_x + k * grow_y + d.x, top - grow_y + d.y),
        ImVec2(right + grow_x + k * grow_y + d.x, top - grow_y + d.y),
        ImVec2(right - cut + grow_x - k * grow_y + d.x, bottom + grow_y + d.y),
        ImVec2(left - grow_x - k * grow_y + d.x, bottom + grow_y + d.y)};
  };
  const float rim = std::clamp(height * 0.08f, 1.0f, 0.55f * unit);
  const float line = std::clamp(height * 0.04f, 1.0f, 0.3f * unit);
  const auto fill = [&](const std::array<ImVec2, 4>& q, float y0, float y1, ImU32 c0, ImU32 c1) {
    const int first = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(q.data(), 4, IM_COL32_WHITE);
    Recolour(dl, first, y0, y1, c0, c1);
  };
  fill(band(0, 0, ImVec2(0.35f * unit, 0.6f * unit)), top, bottom, Rgb(0, 0, 0, 0.36f * alpha),
       Rgb(0, 0, 0, 0.36f * alpha));
  fill(band(0, 0, ImVec2(0, 0)), top, bottom, Fade(p.rim_top, alpha), Fade(p.rim_bottom, alpha));
  fill(band(-rim, -rim, ImVec2(0, 0)), top, bottom, Fade(p.line, alpha), Fade(p.line, alpha));
  const std::array<ImVec2, 4> body = band(-rim - line, -rim - line, ImVec2(0, 0));
  fill(body, body[0].y, body[3].y, Fade(p.top, alpha), Fade(p.bottom, alpha));
  const float bh = body[3].y - body[0].y;
  const float k = cut / height;
  const std::array<ImVec2, 4> shine = {body[0], body[1],
                                       ImVec2(body[1].x - k * bh * 0.48f, body[0].y + bh * 0.48f),
                                       ImVec2(body[0].x - k * bh * 0.48f, body[0].y + bh * 0.48f)};
  fill(shine, body[0].y, body[0].y + bh * 0.48f, Rgb(255, 255, 255, 0.26f * alpha),
       Rgb(255, 255, 255, 0.03f * alpha));
  // The title (and the note under it), centred.
  const float centre = (left + right) / 2;
  if (note.empty())
  {
    const TextLook look = TitleLook(unit, height * 0.6f / unit);
    Text(dl, ImVec2(centre, top + (height - look.px) / 2 + look.px * 0.05f), look, title, alpha,
         Align::Centre);
  }
  else
  {
    const TextLook look = TitleLook(unit, height * 0.48f / unit);
    const TextLook small = LabelLook(unit, height * 0.3f / unit);
    const float y = top + height * 0.1f;
    Text(dl, ImVec2(centre, y), look, title, alpha, Align::Centre);
    Text(dl, ImVec2(centre, y + look.px * 0.95f), small, note, alpha, Align::Centre);
  }
  return box;
}

Box Title(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size, Align align,
          float alpha)
{
  const TextLook look = TitleLook(unit, size);
  const ImVec2 s = Text(dl, pos, look, text, alpha, align);
  const float x = align == Align::Centre ? pos.x - s.x / 2 : align == Align::Right ? pos.x - s.x : pos.x;
  return {ImVec2(x, pos.y), ImVec2(x + s.x, pos.y + s.y)};
}

Box Label(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size, Align align,
          ImU32 colour, float alpha)
{
  TextLook look = LabelLook(unit, size);
  look.top = colour;
  look.bottom = Mix(colour, Rgb(0, 0, 0, static_cast<float>((colour >> 24) & 0xFF) / 255), 0.12f);
  const ImVec2 s = Text(dl, pos, look, text, alpha, align);
  const float x = align == Align::Centre ? pos.x - s.x / 2 : align == Align::Right ? pos.x - s.x : pos.x;
  return {ImVec2(x, pos.y), ImVec2(x + s.x, pos.y + s.y)};
}

Box LabelPlate(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size,
               Align align, Tone tone, float alpha)
{
  const Paint p = PaintOf(tone);
  const TextLook look = Inked(LabelLook(unit, size), p);
  const ImVec2 s = Measure(look, text);
  const float pad_x = 1.4f * unit, pad_y = 0.55f * unit;
  const float w = s.x + 2 * pad_x, h = s.y + 2 * pad_y;
  const float x = align == Align::Centre ? pos.x - w / 2 : align == Align::Right ? pos.x - w : pos.x;
  const Box box{ImVec2(x, pos.y), ImVec2(x + w, pos.y + h)};
  Plate(dl, box.min, box.max, unit, tone, alpha);
  Text(dl, ImVec2(x + pad_x, pos.y + pad_y), look, text, alpha);
  return box;
}

Box Timer(ImDrawList* dl, ImVec2 centre, float unit, int seconds, double now_s, float alpha)
{
  seconds = std::max(0, seconds);
  const std::string text = fmt::format("{}:{:02}", seconds / 60, seconds % 60);
  TextLook look = TitleLook(unit, 4.2f);
  look.italic = 0.08f;
  const bool urgent = seconds <= 5;
  const bool soon = seconds <= 10;
  if (urgent)
  {
    look.top = Rgb(255, 170, 160);
    look.bottom = Rgb(255, 40, 36);
  }
  else if (soon)
  {
    look.top = Rgb(255, 248, 180);
    look.bottom = Rgb(255, 196, 30);
  }
  // A plate as wide as "0:00" always is, so the digits never make it jump.
  const ImVec2 widest = Measure(look, "0:00");
  const float pad_x = 1.3f * unit, pad_y = 0.45f * unit;
  const float w = widest.x + 2 * pad_x, h = widest.y + 2 * pad_y;
  const Box box{ImVec2(centre.x - w / 2, centre.y - h / 2), ImVec2(centre.x + w / 2, centre.y + h / 2)};
  Plate(dl, box.min, box.max, unit, Tone::Navy, alpha);
  if (urgent)
  {
    const float grow = 0.14f * Pulse(now_s);
    look.px *= 1 + grow;
    look.outline *= 1 + grow;
  }
  Text(dl, ImVec2(centre.x, centre.y - look.px / 2 + look.px * 0.04f), look, text, alpha,
       Align::Centre);
  return box;
}

Box Badge(ImDrawList* dl, ImVec2 centre, float unit, std::string_view text, Tone tone, float size,
          float alpha)
{
  const Paint p = PaintOf(tone);
  TextLook look = TitleLook(unit, size);
  look.italic = 0.06f;
  look.outline = std::max(1.0f, look.px * 0.085f);
  look.shadow = ImVec2(0, std::max(1.0f, look.px * 0.05f));
  look = Inked(look, p);
  const ImVec2 s = Measure(look, text);
  const float pad_x = 1.5f * unit, pad_y = 0.55f * unit;
  const ImVec2 min(centre.x - s.x / 2 - pad_x, centre.y - s.y / 2 - pad_y);
  const ImVec2 max(centre.x + s.x / 2 + pad_x, centre.y + s.y / 2 + pad_y);
  Slab(dl, min, max, (max.y - min.y) * 0.34f, unit, p, alpha);
  Text(dl, ImVec2(centre.x, centre.y - s.y / 2 + look.px * 0.03f), look, text, alpha,
       Align::Centre);
  return {min, max};
}

namespace
{
// A small tab: the tone's button with the body face's text, top-left at `pos`.
Box Tab(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, Tone tone, bool right_aligned)
{
  const Paint p = PaintOf(tone);
  const TextLook look = Inked(LabelLook(unit, 2.4f), p);
  const ImVec2 s = Measure(look, text);
  const float pad_x = 0.8f * unit, pad_y = 0.3f * unit;
  const float w = s.x + 2 * pad_x, h = s.y + 2 * pad_y;
  const float x = right_aligned ? pos.x - w : pos.x;
  Slab(dl, ImVec2(x, pos.y), ImVec2(x + w, pos.y + h), h * 0.3f, unit * 0.7f, p, 1);
  Text(dl, ImVec2(x + pad_x, pos.y + pad_y), look, text);
  return {ImVec2(x, pos.y), ImVec2(x + w, pos.y + h)};
}
}  // namespace

void Pointer(ImDrawList* dl, ImVec2 c, float radius, float unit, int port, std::string_view tag,
             bool turn, bool tag_left, double now_s)
{
  const Paint p = PaintOf(PortTone(port));
  const float thick = std::max(2.0f, 0.26f * radius);
  const float rim = std::max(1.0f, 0.12f * radius);
  if (turn)
  {
    // The turn's gold ring, breathing.
    const float k = 0.65f + 0.35f * static_cast<float>(0.5 + 0.5 * std::sin(now_s * 5.0));
    const float r = radius + 0.55f * thick + rim;
    dl->AddCircle(c, r, Rgb(30, 18, 0, 0.9f), 48, 0.9f * thick);
    dl->AddCircle(c, r, Rgb(255, 214, 40, k), 48, 0.55f * thick);
  }
  dl->AddCircleFilled(ImVec2(c.x + 0.15f * unit, c.y + 0.4f * unit), radius + rim,
                      Rgb(0, 0, 0, 0.36f), 48);
  int first = dl->VtxBuffer.Size;
  dl->AddCircleFilled(c, radius + rim, IM_COL32_WHITE, 48);
  Recolour(dl, first, c.y - radius, c.y + radius, p.rim_top, p.rim_bottom);
  dl->AddCircleFilled(c, radius, p.line, 48);
  first = dl->VtxBuffer.Size;
  dl->AddCircleFilled(c, radius - std::max(1.0f, 0.06f * radius), IM_COL32_WHITE, 48);
  Recolour(dl, first, c.y - radius, c.y + radius, p.top, p.bottom);
  // The face: light, so the game's own crosshair under it never shows.
  dl->AddCircleFilled(c, radius - thick, p.line, 48);
  first = dl->VtxBuffer.Size;
  dl->AddCircleFilled(c, radius - thick - std::max(1.0f, 0.05f * radius), IM_COL32_WHITE, 48);
  Recolour(dl, first, c.y - radius, c.y + radius, Rgb(255, 255, 255), Rgb(212, 218, 230));
  for (const ImVec2 d : {ImVec2(1, 0), ImVec2(-1, 0), ImVec2(0, 1), ImVec2(0, -1)})
  {
    const ImVec2 a(c.x + d.x * (radius - 0.5f * thick), c.y + d.y * (radius - 0.5f * thick));
    const ImVec2 b(c.x + d.x * radius * 0.3f, c.y + d.y * radius * 0.3f);
    dl->AddLine(a, b, p.line, 0.7f * thick);
    dl->AddLine(a, b, p.bottom, 0.4f * thick);
  }
  dl->AddCircleFilled(c, std::max(1.5f, 0.15f * radius), p.line, 16);
  dl->AddCircleFilled(c, std::max(1.0f, 0.1f * radius), p.bottom, 16);
  if (!tag.empty())
  {
    const float ty = c.y + radius * 0.55f;
    Tab(dl, ImVec2(tag_left ? c.x - radius * 0.55f : c.x + radius * 0.55f, ty), unit, tag,
        PortTone(port), tag_left);
  }
}

void Frame(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, std::string_view tag,
           bool tag_left, float inset)
{
  const Paint p = PaintOf(tone);
  min = ImVec2(min.x + inset, min.y + inset);
  max = ImVec2(max.x - inset, max.y - inset);
  const float round = 0.7f * unit;
  // Built like the buttons, as a ring: shadow, rim, line, the tone, line.
  dl->AddRect(ImVec2(min.x + 0.15f * unit, min.y + 0.35f * unit),
              ImVec2(max.x + 0.15f * unit, max.y + 0.35f * unit), Rgb(0, 0, 0, 0.32f), round, 0,
              1.2f * unit);
  dl->AddRect(min, max, p.rim_top, round, 0, 1.25f * unit);
  dl->AddRect(min, max, p.line, round, 0, 0.9f * unit);
  dl->AddRect(min, max, p.bottom, round, 0, 0.6f * unit);
  dl->AddRect(min, max, p.top, round, 0, 0.25f * unit);
  if (!tag.empty())
  {
    const TextLook look = LabelLook(unit, 2.4f);
    const float h = Measure(look, tag).y + 0.6f * unit;
    Tab(dl, ImVec2(tag_left ? min.x - 0.2f * unit : max.x + 0.2f * unit, min.y - h * 0.8f), unit,
        tag, tone, !tag_left);
  }
}

void UseAtlasForTests(Face face, const GameFont::Atlas* atlas, ImTextureID texture)
{
  s_test_faces[static_cast<int>(face)] = {atlas, texture};
}

bool DemoOn()
{
  static const bool on = [] {
    const char* v = std::getenv("ORCA_UX_KIT_DEMO");
    return v && v[0] == '1';
  }();
  return on;
}

void DrawDemo(ImDrawList* dl, ImVec2 o, ImVec2 size, float u, double now_s)
{
  const auto at = [&](float fx, float fy) { return ImVec2(o.x + size.x * fx, o.y + size.y * fy); };
  // Top centre: the set's banner, the turn's timer beside it, and one about to run out.
  const Box banner = Banner(dl, at(0.29f, 0).x, at(0.71f, 0).x, o.y + 2.2f * u, 9.0f * u, u,
                            Tone::Navy, "RANKED  GAME 2", "ada 1 – 0 bo · bo bans a stage");
  const float mid = (banner.min.y + banner.max.y) / 2;
  const Box t1 = Timer(dl, ImVec2(banner.max.x + 7.0f * u, mid), u, 24, now_s);
  Timer(dl, ImVec2(t1.max.x + 7.5f * u, mid), u, 4, now_s);
  // Top left: the toasts and a chat line on plates, the ping under them.
  float y = o.y + 2.5f * u;
  y = LabelPlate(dl, ImVec2(o.x + 2.5f * u, y), u, "bo joined on port 2", 2.7f).max.y + 1.0f * u;
  y = LabelPlate(dl, ImVec2(o.x + 2.5f * u, y), u, "bo: gl hf … one more?", 2.7f).max.y +
      1.0f * u;
  Label(dl, ImVec2(o.x + 2.5f * u, y), u, "32 ms · 60 fps", 2.6f);
  // The middle: a title over a label, two pointers, two framed tiles.
  Title(dl, at(0.5f, 0.19f), u, "Waiting for bo…", 5.5f, Align::Centre);
  Label(dl, Add(at(0.5f, 0.19f), ImVec2(0, 6.4f * u)), u, "bo forfeits in 0:18", 3.2f,
        Align::Centre, Rgb(255, 226, 120));
  Frame(dl, at(0.40f, 0.34f), at(0.48f, 0.44f), u, Tone::Red, "ada", true);
  Frame(dl, at(0.52f, 0.34f), at(0.60f, 0.44f), u, Tone::Gold, "ada + bo", true);
  Pointer(dl, at(0.44f, 0.40f), 2.6f * u, u, 0, "P1 ada", true, true, now_s);
  Pointer(dl, at(0.585f, 0.415f), 2.6f * u, u, 1, "P2 bo", false, false, now_s);
  // The lower middle: a results card.
  const ImVec2 c0 = at(0.335f, 0.475f), c1 = at(0.665f, 0.565f);
  Plate(dl, c0, c1, u, Tone::Navy);
  const float cy = (c0.y + c1.y) / 2;
  Title(dl, ImVec2(c0.x + 2.0f * u, cy - 2.4f * u), u, "VICTORY", 4.8f);
  Label(dl, ImVec2(c1.x - 2.0f * u, cy - 3.3f * u), u, "1532 → 1550", 3.2f, Align::Right);
  Badge(dl, ImVec2(c1.x - 6.0f * u, cy + 2.0f * u), u, "+18", Tone::Gold, 2.8f);
  // Over the character select's panels: each name on a plate in the port's colour, LOCKED IN.
  for (int port = 0; port < 2; ++port)
  {
    const float cx = o.x + size.x * (0.236f + 0.177f * static_cast<float>(port));
    LabelPlate(dl, ImVec2(cx, o.y + size.y * 0.58f), u, port == 0 ? "ada · 1532" : "bo · 1498",
               2.8f, Align::Centre, PortTone(port));
    Badge(dl, ImVec2(cx, o.y + size.y * 0.71f), u, "LOCKED IN", PortTone(port), 3.2f);
  }
  Badge(dl, at(0.62f, 0.71f), u, "READY", Tone::Gold, 3.2f);
  Badge(dl, at(0.77f, 0.71f), u, "WAITING", Tone::Grey, 3.2f);
  LabelPlate(dl, at(0.70f, 0.58f), u, "Hold Z to find someone else", 2.6f, Align::Centre, Tone::Green);
}
}  // namespace Orca::UX::Kit
