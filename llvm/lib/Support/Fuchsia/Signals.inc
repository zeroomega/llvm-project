//===- Fuchsia/Signals.cpp - Fuchsia Signals Implementation -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the Fuchsia specific implementation of the Signals class.
//
//===----------------------------------------------------------------------===//

#include "Fuchsia.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <dlfcn.h>
#include <link.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <zircon/errors.h>
#include <zircon/exception.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

using namespace llvm;

static ManagedStatic<sys::SmartMutex<true>> ExceptionMutex;

using SignalHandlerFunctionType = void (*)();
static std::atomic<SignalHandlerFunctionType> InterruptFunction = nullptr;

static ManagedStatic<std::vector<std::string>> FilesToRemove;
static bool RegisteredSignalHandler = false;

namespace {

std::thread SignalHandler;
zx_handle_t SignalHandlerEvent = ZX_HANDLE_INVALID;

static void RemoveFilesToRemove() {
  if (!FilesToRemove.isConstructed())
    return;

  while (!FilesToRemove->empty()) {
    llvm::sys::fs::remove(FilesToRemove->back());
    FilesToRemove->pop_back();
  }
}

static void UnregisterHandler() {
  zx_handle_close(SignalHandlerEvent);
  if (SignalHandler.joinable())
    SignalHandler.join();
}

static void ExceptionHandler() {
  assert(SignalHandlerEvent != ZX_HANDLE_INVALID);

  // This structure is used to ensure we close handles to objects we create in
  // this handler.
  struct ScopedHandle {
    ~ScopedHandle() { zx_handle_close(Handle); }
    zx_handle_t Handle = ZX_HANDLE_INVALID;
  };

  ScopedHandle Channel;
  zx_handle_t Self = zx_process_self();
  zx_task_create_exception_channel(Self, 0, &Channel.Handle);

  zx_object_signal(SignalHandlerEvent, 0, ZX_USER_SIGNAL_0);

  zx_wait_item_t WaitItems[] = {
      {
          .handle = SignalHandlerEvent,
          .waitfor = ZX_SIGNAL_HANDLE_CLOSED,
          .pending = 0,
      },
      {
          .handle = Channel.Handle,
          .waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
          .pending = 0,
      },
  };
  zx_status_t Status = zx_object_wait_many(
      WaitItems, sizeof(WaitItems) / sizeof(WaitItems[0]), ZX_TIME_INFINITE);
  if (Status != ZX_OK || (WaitItems[1].pending & ZX_CHANNEL_READABLE) == 0)
    return;

  zx_exception_info_t ExceptionInfo;
  ScopedHandle Exception;
  Status = zx_channel_read(Channel.Handle, 0, &ExceptionInfo,
                           &Exception.Handle, sizeof(ExceptionInfo), 1,
                           nullptr, nullptr);

  // TODO: Check the exception type?

  {
    sys::SmartScopedLock<true> Guard(*ExceptionMutex);
    RemoveFilesToRemove();

    if (auto OldInterruptFunction = InterruptFunction.exchange(nullptr))
      return OldInterruptFunction();
  }

  llvm::sys::RunSignalHandlers();
}

static void RegisterHandler() {
  static ManagedStatic<sys::SmartMutex<true>> SignalHandlerRegistrationMutex;
  sys::SmartScopedLock<true> Guard(*SignalHandlerRegistrationMutex);

  if (RegisteredSignalHandler)
    return;

  zx_event_create(0, &SignalHandlerEvent);
  SignalHandler = std::thread(ExceptionHandler);
  zx_status_t Status = zx_object_wait_one(SignalHandlerEvent, ZX_USER_SIGNAL_0,
                                          ZX_TIME_INFINITE, nullptr);
  if (Status != ZX_OK) {
    assert(false && "zx_port_create failed");
    return;
  }
  std::atexit(UnregisterHandler);

  RegisteredSignalHandler = true;
}

}

// The public API
void llvm::sys::unregisterHandlers() {
  UnregisterHandler();
}

// The public API
void llvm::sys::CleanupOnSignal(uintptr_t Context) {
  {
    sys::SmartScopedLock<true> Guard(*ExceptionMutex);
    RemoveFilesToRemove();
  }
  llvm::sys::RunSignalHandlers();
}

// The public API
void llvm::sys::RunInterruptHandlers() {
  sys::SmartScopedLock<true> Guard(*ExceptionMutex);
  RemoveFilesToRemove();
}

// The public API
bool llvm::sys::RemoveFileOnSignal(StringRef Filename, std::string* ErrMsg) {
  {
    sys::SmartScopedLock<true> Guard(*ExceptionMutex);
    FilesToRemove->push_back(std::string(Filename));
  }
  RegisterHandler();
  return false;
}

// The public API
void llvm::sys::DontRemoveFileOnSignal(StringRef Filename) {
  sys::SmartScopedLock<true> Guard(*ExceptionMutex);
  std::vector<std::string>::reverse_iterator I =
      find(reverse(*FilesToRemove), Filename);
  if (I != FilesToRemove->rend())
    FilesToRemove->erase(I.base() - 1);
}

// The public API
void llvm::sys::PrintStackTraceOnErrorSignal(StringRef Argv0,
                                             bool DisableCrashReporting) {}

// The public API
void llvm::sys::DisableSystemDialogsOnCrash() {}

// The public API
void llvm::sys::PrintStackTrace(raw_ostream &OS, int Depth);

// The public API
void llvm::sys::AddSignalHandler(llvm::sys::SignalHandlerCallback FnPtr, void *Cookie) {
  insertSignalHandler(FnPtr, Cookie);
  RegisterHandler();
}

struct DlIteratePhdrData {
  void **StackTrace;
  int depth;
  bool first;
  const char **modules;
  intptr_t *offsets;
  const char *main_exec_name;
};

static int dl_iterate_phdr_cb(dl_phdr_info *info, size_t size, void *arg) {
  DlIteratePhdrData *data = (DlIteratePhdrData *)arg;
  const char *name = data->first ? data->main_exec_name : info->dlpi_name;
  data->first = false;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const auto *phdr = &info->dlpi_phdr[i];
    if (phdr->p_type != PT_LOAD)
      continue;
    intptr_t beg = info->dlpi_addr + phdr->p_vaddr;
    intptr_t end = beg + phdr->p_memsz;
    for (int j = 0; j < data->depth; j++) {
      if (data->modules[j])
        continue;
      intptr_t addr = (intptr_t)data->StackTrace[j];
      if (beg <= addr && addr < end) {
        data->modules[j] = name;
        data->offsets[j] = addr - info->dlpi_addr;
      }
    }
  }
  return 0;
}

static bool findModulesAndOffsets(void **StackTrace, int Depth,
                                  const char **Modules, intptr_t *Offsets,
                                  const char *MainExecutableName,
                                  StringSaver &StrPool) {
  DlIteratePhdrData data = {StackTrace, Depth,   true,
                            Modules,    Offsets, MainExecutableName};
  dl_iterate_phdr(dl_iterate_phdr_cb, &data);
  return true;
}

/// This function registers a function to be called when the user "interrupts"
/// the program (typically by pressing ctrl-c).  When the user interrupts the
/// program, the specified interrupt function is called instead of the program
/// being killed, and the interrupt function automatically disabled.
///
/// Note that interrupt functions are not allowed to call any non-reentrant
/// functions.  An null interrupt function pointer disables the current
/// installed function.  Note also that the handler may be executed on a
/// different thread on some platforms.
void llvm::sys::SetInterruptFunction(void (*IF)()) {
  RegisterHandler();
  InterruptFunction.exchange(IF);
}

// The public API
void llvm::sys::SetInfoSignalFunction(void (*Handler)()) {}

// The public API
void llvm::sys::SetOneShotPipeSignalFunction(void (*Handler)()) {}

// The public API
void llvm::sys::DefaultOneShotPipeSignalHandler() {}
