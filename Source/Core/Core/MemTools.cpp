// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MemTools.h"

#include <cstdlib>

#include "Common/CommonFuncs.h"
#include "Common/MsgHandler.h"

#include "Core/MachineContext.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/Rollback/Cow.h"
#include "Core/System.h"

#if defined(__FreeBSD__) || defined(__NetBSD__)
#include <signal.h>
#endif
#ifndef _WIN32
#include <unistd.h>  // Needed for _POSIX_VERSION
#endif

#if defined(_WIN32) && defined(_M_X64)
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include <immintrin.h>
#include <intrin.h>
#endif
#ifdef _WIN32

#include "Common/Assert.h"
#endif
#if defined(__APPLE__) && !defined(USE_SIGACTION_ON_APPLE)
#include <mutex>

#include <signal.h>

#include "Common/Thread.h"
#endif

#if defined(__APPLE__)
#ifdef _M_X86_64
#define THREAD_STATE64_COUNT x86_THREAD_STATE64_COUNT
#define THREAD_STATE64 x86_THREAD_STATE64
#define thread_state64_t x86_thread_state64_t
#elif defined(_M_ARM_64)
#define THREAD_STATE64_COUNT ARM_THREAD_STATE64_COUNT
#define THREAD_STATE64 ARM_THREAD_STATE64
#define thread_state64_t arm_thread_state64_t
#else
#error Unsupported architecture
#endif
#endif

namespace EMM
{
#ifdef _WIN32

static PVOID s_veh_handle;

#ifdef _M_X64
// The vector register bits above XMM (the AVX and AVX-512 state) are not restored from the context a
// handled fault resumes with, and the snapshot fault handler's calls (memcpy, VirtualProtect) may
// change them: they are volatile across calls. A host copy into guest RAM with 256-bit registers
// that faulted on a protected page would then go on storing those upper halves as zeros. So that
// state is saved before the snapshot handler runs and put back after it. x87 and SSE come back
// from the context; AMX tiles are granted per thread and left out.
constexpr std::size_t VECTOR_STATE_BYTES = 8192;
// Zero-initialised, as XRSTOR wants the save header's reserved bytes; aligned to 64 by hand.
static thread_local u8 t_vector_state[VECTOR_STATE_BYTES + 64];
static u64 s_vector_state_mask = 0;

static void* VectorStateArea()
{
  const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(t_vector_state);
  return reinterpret_cast<void*>((at + 63) & ~std::uintptr_t{63});
}

static u64 VectorStateMask()
{
  int r[4];
  __cpuid(r, 1);
  if (!(r[2] & (1 << 27)))  // OSXSAVE
    return 0;
  const u64 mask = _xgetbv(0) & ~u64{3} & ~((u64{1} << 17) | (u64{1} << 18));
  std::size_t end = 512 + 64;
  for (int bit = 2; bit < 32; ++bit)
  {
    if (!(mask >> bit & 1))
      continue;
    __cpuidex(r, 0xD, bit);
    end = std::max<std::size_t>(end, std::size_t{static_cast<u32>(r[1])} + static_cast<u32>(r[0]));
  }
  return end <= VECTOR_STATE_BYTES ? mask : 0;
}

// DIAGNOSTIC build only. YG_DIAG_COW_VECTOR_AUDIT=1 counts write faults, whether the fault's
// context carries the AVX state, whether it matches the live registers on entry, and whether the
// snapshot handler changed the YMM upper halves. YG_DIAG_COW_CLOBBER_VECTORS=1 zeroes the vector
// registers before the snapshot handler, to show whether a resumed copy keeps what it loaded.
static bool s_audit_vectors = false;
static bool s_clobber_vectors = false;
static u32 s_ymm_offset = 0;
static bool s_xinuse = false;  // XGETBV with ECX=1
static std::atomic<u64> s_audit_faults{0}, s_audit_context_avx{0}, s_audit_context_differs{0},
    s_audit_handler_changed{0}, s_audit_avx512_in_use{0}, s_audit_context_no_avx512{0};
constexpr std::size_t AUDIT_BYTES = 1024;
static thread_local u8 t_audit_before[AUDIT_BYTES + 64];
static thread_local u8 t_audit_after[AUDIT_BYTES + 64];

static u8* Aligned64(u8* p)
{
  const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(p);
  return reinterpret_cast<u8*>((at + 63) & ~std::uintptr_t{63});
}

// The live YMM upper halves (16 x 16 bytes), zeros when the AVX state is in its initial state.
static void LiveYmmHigh(u8* area, u8 out[256])
{
  _xsave64(area, 4);
  u64 bv;
  std::memcpy(&bv, area + 512, sizeof(bv));
  if (bv & 4)
    std::memcpy(out, area + s_ymm_offset, 256);
  else
    std::memset(out, 0, 256);
}

static void AuditBefore(CONTEXT* ctx, u8 live[256])
{
  s_audit_faults.fetch_add(1, std::memory_order_relaxed);
  // Whether AVX-512 state (opmask, ZMM_Hi256, Hi16_ZMM) is in use, read before anything here runs.
  const bool avx512 = s_xinuse && (_xgetbv(1) & 0xe0) != 0;
  LiveYmmHigh(Aligned64(t_audit_before), live);
  DWORD64 features = 0;
  const bool has_xstate = (ctx->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE &&
                          GetXStateFeaturesMask(ctx, &features);
  if (avx512)
  {
    s_audit_avx512_in_use.fetch_add(1, std::memory_order_relaxed);
    if (!has_xstate || (features & 0xe0) != 0xe0)
      s_audit_context_no_avx512.fetch_add(1, std::memory_order_relaxed);
  }
  if (has_xstate && (features & XSTATE_MASK_AVX))
  {
    s_audit_context_avx.fetch_add(1, std::memory_order_relaxed);
    DWORD length = 0;
    const void* ymm = LocateXStateFeature(ctx, XSTATE_AVX, &length);
    if (ymm && length >= 256 && std::memcmp(ymm, live, 256) != 0)
      s_audit_context_differs.fetch_add(1, std::memory_order_relaxed);
  }
}

static void AuditAfter(const u8 before[256])
{
  u8 after[256];
  LiveYmmHigh(Aligned64(t_audit_after), after);
  if (std::memcmp(before, after, 256) != 0)
    s_audit_handler_changed.fetch_add(1, std::memory_order_relaxed);
}
#endif

static LONG NTAPI Handler(PEXCEPTION_POINTERS pPtrs)
{
  switch (pPtrs->ExceptionRecord->ExceptionCode)
  {
  case EXCEPTION_ACCESS_VIOLATION:
  {
    ULONG_PTR access_type = pPtrs->ExceptionRecord->ExceptionInformation[0];
    if (access_type == 8)  // Rule out DEP
    {
      return EXCEPTION_CONTINUE_SEARCH;
    }

    // virtual address of the inaccessible data
    uintptr_t fault_address = (uintptr_t)pPtrs->ExceptionRecord->ExceptionInformation[1];
    SContext* ctx = pPtrs->ContextRecord;

    // A write to guest RAM that a rollback snapshot protected: the page is writable again and the
    // write runs again. Before the JIT, which would backpatch a fastmem write it doesn't own.
    if (access_type == 1)
    {
#ifdef _M_X64
      const u64 mask = s_vector_state_mask;
      void* const state = mask ? VectorStateArea() : nullptr;
      if (mask)
        _xsave64(state, mask);
      alignas(16) u8 audit_live[256];
      if (s_audit_vectors)
        AuditBefore(ctx, audit_live);
      if (s_clobber_vectors)
        _mm256_zeroall();
#endif
      const bool handled = Rollback::Cow::HandleFault(fault_address);
#ifdef _M_X64
      if (s_audit_vectors && !s_clobber_vectors)
        AuditAfter(audit_live);
      if (mask)
        _xrstor64(state, mask);
#endif
      if (handled)
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (Core::System::GetInstance().GetJitInterface().HandleFault(fault_address, ctx))
    {
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    else
    {
      // Let's not prevent debugging.
      return EXCEPTION_CONTINUE_SEARCH;
    }
  }

  case EXCEPTION_STACK_OVERFLOW:
    if (Core::System::GetInstance().GetJitInterface().HandleStackFault())
      return EXCEPTION_CONTINUE_EXECUTION;
    else
      return EXCEPTION_CONTINUE_SEARCH;

  case EXCEPTION_ILLEGAL_INSTRUCTION:
    // No SSE support? Or simply bad codegen?
    return EXCEPTION_CONTINUE_SEARCH;

  case EXCEPTION_PRIV_INSTRUCTION:
    // okay, dynarec codegen is obviously broken.
    return EXCEPTION_CONTINUE_SEARCH;

  case EXCEPTION_IN_PAGE_ERROR:
    // okay, something went seriously wrong, out of memory?
    return EXCEPTION_CONTINUE_SEARCH;

  case EXCEPTION_BREAKPOINT:
    // might want to do something fun with this one day?
    return EXCEPTION_CONTINUE_SEARCH;

  default:
    return EXCEPTION_CONTINUE_SEARCH;
  }
}

void InstallExceptionHandler()
{
  ASSERT(!s_veh_handle);
#ifdef _M_X64
  s_vector_state_mask = VectorStateMask();
  // DIAGNOSTIC build only: YG_DIAG_COW_KEEP_VECTORS=0 runs the snapshot handler without keeping
  // the vector state, to show the loss it causes.
  if (const char* v = std::getenv("YG_DIAG_COW_KEEP_VECTORS"); v && v[0] == '0')
    s_vector_state_mask = 0;
  int id[4];
  __cpuid(id, 1);
  const bool avx = (id[2] & (1 << 27)) && (id[2] & (1 << 28)) && (_xgetbv(0) & 6) == 6;
  if (avx)
  {
    int r[4];
    __cpuidex(r, 0xD, 2);
    s_ymm_offset = static_cast<u32>(r[1]);
    __cpuidex(r, 0xD, 1);
    s_xinuse = (r[0] & (1 << 2)) != 0;
  }
  const auto on = [](const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '1';
  };
  s_audit_vectors = avx && s_ymm_offset + 256 <= AUDIT_BYTES && on("YG_DIAG_COW_VECTOR_AUDIT");
  s_clobber_vectors = avx && on("YG_DIAG_COW_CLOBBER_VECTORS");
  s_audit_faults = 0;
  s_audit_context_avx = 0;
  s_audit_context_differs = 0;
  s_audit_handler_changed = 0;
  s_audit_avx512_in_use = 0;
  s_audit_context_no_avx512 = 0;
#endif
  s_veh_handle = AddVectoredExceptionHandler(TRUE, Handler);
  ASSERT(s_veh_handle);
}

void UninstallExceptionHandler()
{
  ULONG status = RemoveVectoredExceptionHandler(s_veh_handle);
  ASSERT(status);
  if (status)
    s_veh_handle = nullptr;
}

bool IsExceptionHandlerSupported()
{
  return true;
}

CowVectorAudit GetCowVectorAudit()
{
#ifdef _M_X64
  return {s_audit_vectors, s_clobber_vectors, s_audit_faults.load(), s_audit_context_avx.load(),
          s_audit_context_differs.load(), s_audit_handler_changed.load(),
          s_audit_avx512_in_use.load(), s_audit_context_no_avx512.load()};
#else
  return {};
#endif
}

void InstallCowFallbackHandler()
{
  // The vectored handler sees every thread's faults.
}

#elif defined(__APPLE__) && !defined(USE_SIGACTION_ON_APPLE)

static void CheckKR(const char* name, kern_return_t kr)
{
  if (kr)
  {
    PanicAlertFmt("{} failed: kr={:x}", name, kr);
  }
}

static void ExceptionThread(mach_port_t port)
{
  Common::SetCurrentThreadName("Mach exception thread");
#pragma pack(4)
  struct alignas(8)
  {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    exception_type_t exception;
    mach_msg_type_number_t codeCnt;
    int64_t code[2];
    int flavor;
    mach_msg_type_number_t old_stateCnt;
    natural_t old_state[THREAD_STATE64_COUNT];
    mach_msg_trailer_t trailer;
  } msg_in;

  struct alignas(8)
  {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    kern_return_t RetCode;
    int flavor;
    mach_msg_type_number_t new_stateCnt;
    natural_t new_state[THREAD_STATE64_COUNT];
  } msg_out;
#pragma pack()
  memset(&msg_in, 0xee, sizeof(msg_in));
  memset(&msg_out, 0xee, sizeof(msg_out));
  mach_msg_size_t send_size = 0;
  mach_msg_option_t option = MACH_RCV_MSG;
  while (true)
  {
    // If this isn't the first run, send the reply message.  Then, receive
    // a message: either a mach_exception_raise_state RPC due to
    // thread_set_exception_ports, or MACH_NOTIFY_NO_SENDERS due to
    // mach_port_request_notification.
    CheckKR("mach_msg_overwrite",
            mach_msg_overwrite(&msg_out.Head, option, send_size, sizeof(msg_in), port,
                               MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL, &msg_in.Head, 0));

    if (msg_in.Head.msgh_id == MACH_NOTIFY_NO_SENDERS)
    {
      // the other thread exited
      mach_port_destroy(mach_task_self(), port);
      return;
    }

    if (msg_in.Head.msgh_id != 2406)
    {
      PanicAlertFmt("unknown message received");
      return;
    }

    if (msg_in.flavor != THREAD_STATE64)
    {
      PanicAlertFmt("unknown flavor {} (expected {})", msg_in.flavor, THREAD_STATE64);
      return;
    }

    thread_state64_t* state = (thread_state64_t*)msg_in.old_state;

    // A write to guest RAM that a rollback snapshot protected: the page is writable again and the
    // thread runs the write again with its state unchanged. Before the JIT, which would backpatch
    // a fastmem write it doesn't own.
    bool ok = Rollback::Cow::HandleFault((uintptr_t)msg_in.code[1]) ||
              Core::System::GetInstance().GetJitInterface().HandleFault((uintptr_t)msg_in.code[1],
                                                                        state);

    // Set up the reply.
    msg_out.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(msg_in.Head.msgh_bits), 0);
    msg_out.Head.msgh_remote_port = msg_in.Head.msgh_remote_port;
    msg_out.Head.msgh_local_port = MACH_PORT_NULL;
    msg_out.Head.msgh_id = msg_in.Head.msgh_id + 100;
    msg_out.NDR = msg_in.NDR;
    if (ok)
    {
      msg_out.RetCode = KERN_SUCCESS;
      msg_out.flavor = THREAD_STATE64;
      msg_out.new_stateCnt = THREAD_STATE64_COUNT;
      memcpy(msg_out.new_state, msg_in.old_state, THREAD_STATE64_COUNT * sizeof(natural_t));
    }
    else
    {
      // Pass the exception to the next handler (debugger or crash).
      msg_out.RetCode = KERN_FAILURE;
      msg_out.flavor = 0;
      msg_out.new_stateCnt = 0;
    }
    msg_out.Head.msgh_size =
        offsetof(__typeof__(msg_out), new_state) + msg_out.new_stateCnt * sizeof(natural_t);

    send_size = msg_out.Head.msgh_size;
    option |= MACH_SEND_MSG;
  }
}

void InstallExceptionHandler()
{
  mach_port_t port;
  CheckKR("mach_port_allocate",
          mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
  std::thread exc_thread(ExceptionThread, port);
  exc_thread.detach();
  // Obtain a send right for thread_set_exception_ports to copy...
  CheckKR("mach_port_insert_right",
          mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
  // Mach tries the following exception ports in order: thread, task, host.
  // Debuggers set the task port, so we grab the thread port.
  CheckKR("thread_set_exception_ports",
          thread_set_exception_ports(mach_thread_self(), EXC_MASK_BAD_ACCESS, port,
                                     EXCEPTION_STATE | MACH_EXCEPTION_CODES, THREAD_STATE64));
  // ...and get rid of our copy so that MACH_NOTIFY_NO_SENDERS works.
  CheckKR("mach_port_mod_refs",
          mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, -1));
  mach_port_t previous;
  CheckKR("mach_port_request_notification",
          mach_port_request_notification(mach_task_self(), port, MACH_NOTIFY_NO_SENDERS, 0, port,
                                         MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous));
}

void UninstallExceptionHandler()
{
}

bool IsExceptionHandlerSupported()
{
  return true;
}

CowVectorAudit GetCowVectorAudit()
{
  return {};
}

// Faults on threads without the Mach thread port (only the CPU thread has it) reach the task's
// and host's ports, which nothing here claims, and then become SIGBUS (a write to a read-only page)
// or SIGSEGV.
static struct sigaction s_cow_old_segv;
static struct sigaction s_cow_old_bus;

static void CowSignalHandler(int sig, siginfo_t* info, void* raw_context)
{
  if (Rollback::Cow::HandleFault(reinterpret_cast<uintptr_t>(info->si_addr)))
    return;
  struct sigaction& old_sa = sig == SIGBUS ? s_cow_old_bus : s_cow_old_segv;
  if ((old_sa.sa_flags & SA_SIGINFO) && old_sa.sa_sigaction)
  {
    old_sa.sa_sigaction(sig, info, raw_context);
    return;
  }
  if (!(old_sa.sa_flags & SA_SIGINFO) && old_sa.sa_handler != SIG_DFL &&
      old_sa.sa_handler != SIG_IGN)
  {
    old_sa.sa_handler(sig);
    return;
  }
  // The default action: put it back and let the faulting instruction run again.
  struct sigaction default_sa{};
  default_sa.sa_handler = SIG_DFL;
  sigemptyset(&default_sa.sa_mask);
  sigaction(sig, &default_sa, nullptr);
}

void InstallCowFallbackHandler()
{
  static std::once_flag s_installed;
  std::call_once(s_installed, [] {
    struct sigaction sa{};
    sa.sa_sigaction = &CowSignalHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_cow_old_segv);
    sigaction(SIGBUS, &sa, &s_cow_old_bus);
  });
}

#elif defined(_POSIX_VERSION) && !defined(_M_GENERIC)

static struct sigaction old_sa_segv;
#if defined(__APPLE__)
static struct sigaction old_sa_bus;
#endif

static void sigsegv_handler(int sig, siginfo_t* info, void* raw_context)
{
  if (sig != SIGSEGV
#if defined(__APPLE__)
      && sig != SIGBUS
#endif
  )
  {
    // We are not interested in other signals - handle it as usual.
    return;
  }
  auto* const context = static_cast<ucontext_t*>(raw_context);
  const int sicode = info->si_code;
  if (sicode != SEGV_MAPERR && sicode != SEGV_ACCERR)
  {
    // Huh? Return.
    return;
  }
  const auto bad_address = reinterpret_cast<uintptr_t>(info->si_addr);

// Get all the information we can out of the context.
#ifdef __OpenBSD__
  SContext* const ctx = context;
#elif defined(__APPLE__)
  // `uc_mcontext` is already a pointer here.
  SContext* const ctx = context->uc_mcontext;
#else
  SContext* const ctx = &context->uc_mcontext;
#endif
  // A write to guest RAM that a rollback snapshot protected: the page is writable again and the
  // write runs again. Before the JIT, which would backpatch a fastmem write it doesn't own.
  if (sicode == SEGV_ACCERR && Rollback::Cow::HandleFault(bad_address))
    return;
  if (Core::System::GetInstance().GetJitInterface().HandleFault(bad_address, ctx))
    return;

  // If JIT didn't handle the signal, restore the original handler and invoke it.
  const auto& old_sa =
#if defined(__APPLE__)
      (sig == SIGBUS) ? old_sa_bus :
#endif
                        old_sa_segv;

  sigaction(sig, &old_sa, nullptr);
  raise(sig);
}

void InstallExceptionHandler()
{
  stack_t signal_stack;
#ifdef __FreeBSD__
  signal_stack.ss_sp = (char*)malloc(SIGSTKSZ);
#else
  signal_stack.ss_sp = malloc(SIGSTKSZ);
#endif
  signal_stack.ss_size = SIGSTKSZ;
  signal_stack.ss_flags = 0;
  if (sigaltstack(&signal_stack, nullptr) != 0)
    PanicAlertFmt("sigaltstack failed: {}", Common::LastStrerrorString());
  struct sigaction sa{};
  sa.sa_sigaction = &sigsegv_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &old_sa_segv);
#ifdef __APPLE__
  sigaction(SIGBUS, &sa, &old_sa_bus);
#endif
}

void UninstallExceptionHandler()
{
  stack_t signal_stack;
  stack_t old_stack;
  signal_stack.ss_flags = SS_DISABLE;
  if (!sigaltstack(&signal_stack, &old_stack) && !(old_stack.ss_flags & SS_DISABLE))
  {
    free(old_stack.ss_sp);
  }
  sigaction(SIGSEGV, &old_sa_segv, nullptr);
#ifdef __APPLE__
  sigaction(SIGBUS, &old_sa_bus, nullptr);
#endif
}

bool IsExceptionHandlerSupported()
{
  return true;
}

CowVectorAudit GetCowVectorAudit()
{
  return {};
}

void InstallCowFallbackHandler()
{
  // The signal handler is process-wide.
}

#else  // _M_GENERIC or unsupported platform

void InstallExceptionHandler()
{
}

void UninstallExceptionHandler()
{
}

bool IsExceptionHandlerSupported()
{
  return false;
}

CowVectorAudit GetCowVectorAudit()
{
  return {};
}

void InstallCowFallbackHandler()
{
}

#endif

}  // namespace EMM
