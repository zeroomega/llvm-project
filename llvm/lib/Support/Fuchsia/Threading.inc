//===- Fuchsia/Threading.inc - Fuchsia Threading Implementation -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the Fuchsia specific implementation of Threading functions.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"

#include <zircon/process.h>
#include <zircon/syscalls.h>

namespace {
  #include <threads.h>
}

namespace llvm {

thrd_t
llvm_execute_on_thread_impl(int (*ThreadFunc)(void *), void *Arg,
                            std::optional<unsigned> /*StackSizeInBytes*/) {
  thrd_t Thread;
  if (::thrd_create(&Thread, ThreadFunc, Arg) != thrd_success)
    report_fatal_error("thrd_create failed");
  return Thread;
}

void llvm_thread_detach_impl(thrd_t Thread) {
  if (::thrd_detach(Thread) != thrd_success)
    report_fatal_error("thrd_detach failed");
}

void llvm_thread_join_impl(thrd_t Thread) {
  [[maybe_unused]] int res;
  if (::thrd_join(Thread, &res) != thrd_success)
    report_fatal_error("thrd_join failed");
}

thrd_t llvm_thread_get_id_impl(thrd_t Thread) { return Thread; }

thrd_t llvm_thread_get_current_id_impl() { return ::thrd_current(); }

} // namespace llvm

uint64_t llvm::get_threadid() {
  return static_cast<uint64_t>(::thrd_current());
}

static constexpr uint32_t get_max_thread_name_length_impl() {
  return ZX_MAX_NAME_LEN;
}

uint32_t llvm::get_max_thread_name_length() {
  return get_max_thread_name_length_impl();
}

void llvm::set_thread_name(const Twine &Name) {
  // Make sure the input is null terminated.
  SmallString<64> Storage;
  StringRef NameStr = Name.toNullTerminatedStringRef(Storage);

  // Truncate from the beginning, not the end, if the specified name is too
  // long.  For one, this ensures that the resulting string is still null
  // terminated, but additionally the end of a long thread name will usually
  // be more unique than the beginning, since a common pattern is for similar
  // threads to share a common prefix.
  if (get_max_thread_name_length() > 0)
    NameStr = NameStr.take_back(get_max_thread_name_length());
  zx_handle_t handle = zx_thread_self();
  zx_object_set_property(handle, ZX_PROP_NAME, NameStr.data(), NameStr.size());
}

void llvm::get_thread_name(SmallVectorImpl<char> &Name) {
  Name.clear();
  zx_handle_t handle = zx_thread_self();
  constexpr uint32_t len = get_max_thread_name_length_impl();
  char Buffer[len] = {'\0'};
  if (zx_object_get_property(handle, ZX_PROP_NAME, Buffer, len) == ZX_OK)
    Name.append(Buffer, Buffer + strlen(Buffer));
}

static int computeHostNumHardwareThreads() {
  return zx_system_get_num_cpus();
}

void llvm::ThreadPoolStrategy::apply_thread_strategy(
    unsigned ThreadPoolNum) const {}

unsigned llvm::get_cpus() { return 1; }

int llvm::get_physical_cores() {
  static int NumCores = zx_system_get_num_cpus();
  return NumCores;
}
