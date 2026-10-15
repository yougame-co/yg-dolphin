// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <span>

#include "Common/CommonTypes.h"

namespace Orca::UX
{
class GuestMemory;
}

namespace Rollback
{
// Replay verification alone discounts UI text writes. Remember each byte's pre-text value,
// rather than masking a whole tag record, text buffer, heap allocation or RAM page. Shorter
// names then hash identically too, including the bytes left behind by an earlier longer line.
// This state is local, never serialized, and travels with local rollback/seek checkpoints.
class ReplayText
{
public:
  class Scope
  {
  public:
    explicit Scope(ReplayText& text) : m_previous(s_current) { s_current = &text; }
    ~Scope() { s_current = m_previous; }

  private:
    ReplayText* m_previous;
  };

  static void Write8(Orca::UX::GuestMemory& memory, u32 address, u8 value);
  static void Write16(Orca::UX::GuestMemory& memory, u32 address, u16 value);
  static void Write32(Orca::UX::GuestMemory& memory, u32 address, u32 value);
  // The current byte with only a still-present text write discounted. A game write that
  // replaces it is covered normally; no guest RAM is modified for verification.
  u8 VerifiedByte(u32 address, u8 value) const;
  u64 Checksum(std::span<const u8> mem1, std::span<const u8> mem2) const;

private:
  struct Byte
  {
    u8 before;
    u8 text;
  };
  std::shared_ptr<std::map<u32, Byte>> m_bytes = std::make_shared<std::map<u32, Byte>>();
  static inline thread_local ReplayText* s_current = nullptr;
};
}  // namespace Rollback
