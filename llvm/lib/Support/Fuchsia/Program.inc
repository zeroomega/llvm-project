//===- Fuchsia/Program.cpp - Fuchsia Program Implementation -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Fuchsia specific portion of the Program class.
//
//===----------------------------------------------------------------------===//

#include "Fuchsia.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Config/config.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>

#include <lib/fdio/spawn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zircon/errors.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

extern char **environ;

namespace llvm {

using namespace sys;

ProcessInfo::ProcessInfo() : Pid(0), Process(ZX_HANDLE_INVALID), ReturnCode(0) {}

ErrorOr<std::string> sys::findProgramByName(StringRef Name,
                                            ArrayRef<StringRef> Paths) {
  assert(!Name.empty() && "Must have a name!");
  // Use the given path verbatim if it contains any slashes; this matches
  // the behavior of sh(1) and friends.
  if (Name.find('/') != StringRef::npos)
    return std::string(Name);

  SmallVector<StringRef, 16> EnvironmentPaths;
  if (Paths.empty())
    if (const char *PathEnv = std::getenv("PATH")) {
      SplitString(PathEnv, EnvironmentPaths, ":");
      Paths = EnvironmentPaths;
    }

  for (auto Path : Paths) {
    if (Path.empty())
      continue;

    // Check to see if this first directory contains the executable...
    SmallString<128> FilePath(Path);
    sys::path::append(FilePath, Name);
    if (sys::fs::can_execute(FilePath.c_str()))
      return std::string(FilePath.str()); // Found the executable!
  }
  return errc::no_such_file_or_directory;
}

static bool RedirectIO(std::optional<StringRef> Path, int FD, std::string *ErrMsg,
                       SmallVectorImpl<fdio_spawn_action_t> &SpawnActions) {
  if (!Path) // Noop
    return false;
  if (Path->empty())
    return true;

  std::string File = Path->str();

  // Open the file
  int InFD = open(File.c_str(), FD == 0 ? O_RDONLY : O_WRONLY|O_CREAT, 0666);
  if (InFD == -1) {
    return MakeErrMsg(ErrMsg, "Cannot open file '" + File + "' for "
                      + (FD == 0 ? "input" : "output"));
    return false;
  }

  // Install it as the requested FD
  SpawnActions.push_back(fdio_spawn_action{
    .action = FDIO_SPAWN_ACTION_TRANSFER_FD,
    .fd = { .local_fd = InFD, .target_fd = FD },
  });

  return true;
}

}

static std::vector<const char *>
toNullTerminatedCStringArray(ArrayRef<StringRef> Strings, StringSaver &Saver) {
  std::vector<const char *> Result;
  for (StringRef S : Strings)
    Result.push_back(Saver.save(S).data());
  Result.push_back(nullptr);
  return Result;
}

static bool Execute(ProcessInfo &PI, StringRef Program,
                    ArrayRef<StringRef> Args, std::optional<ArrayRef<StringRef>> Env,
                    ArrayRef<std::optional<StringRef>> Redirects,
                    unsigned MemoryLimit, std::string *ErrMsg,
                    BitVector *AffinityMask) {
  if (!llvm::sys::fs::exists(Program)) {
    if (ErrMsg)
      *ErrMsg = std::string("Executable \"") + Program.str() +
                std::string("\" doesn't exist!");
    return false;
  }

  BumpPtrAllocator Allocator;
  StringSaver Saver(Allocator);
  std::vector<const char *> ArgVector, EnvVector;
  const char **Argv = nullptr;
  const char **Envp = nullptr;
  ArgVector = toNullTerminatedCStringArray(Args, Saver);
  Argv = ArgVector.data();
  if (Env) {
    EnvVector = toNullTerminatedCStringArray(*Env, Saver);
    Envp = EnvVector.data();
  }

  SmallVector<fdio_spawn_action_t, 3> SpawnActions;
  if (!Redirects.empty()) {
    assert(Redirects.size() == 3);
    // Redirect stdin
    if (RedirectIO(Redirects[0], STDIN_FILENO, ErrMsg, SpawnActions))
      return false;
    // Redirect stdout
    if (RedirectIO(Redirects[1], STDOUT_FILENO, ErrMsg, SpawnActions))
      return false;
    // Redirect stderr
    if (RedirectIO(Redirects[2], STDOUT_FILENO, ErrMsg, SpawnActions))
      return false;
  }

  char ErrorMsg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  zx_handle_t ProcessHandle = ZX_HANDLE_INVALID;
  zx_status_t Status = fdio_spawn_etc(
      ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL & (~FDIO_SPAWN_CLONE_STDIO),
      Program.str().c_str(), Argv, Envp, SpawnActions.size(), &SpawnActions[0],
      &ProcessHandle, ErrorMsg);
  if (Status != ZX_OK) {
    MakeErrMsg(ErrMsg, ErrorMsg, Status);
    return false;
  }

  zx_info_handle_basic_t Info;
  Status = zx_object_get_info(ProcessHandle, ZX_INFO_HANDLE_BASIC, &Info,
                              sizeof(Info), NULL, NULL);
  if (Status != ZX_OK) {
    MakeErrMsg(ErrMsg, ErrorMsg, Status);
    return false;
  }

  PI.Process = ProcessHandle;
  PI.Pid = Info.koid;

  return true;
}

namespace llvm {

ProcessInfo sys::Wait(const ProcessInfo &PI,
                      std::optional<unsigned> SecondsToWait,
                      std::string *ErrMsg,
                      std::optional<ProcessStatistics> *ProcStat,
                      bool Polling) {
  assert(PI.Process && "invalid pid to wait on, process not started?");
  //assert(PI.ProcessHandle &&
  //       "invalid process handle to wait on, process not started?");

  auto CloseOnExit = make_scope_exit([&]() { zx_handle_close(PI.Process); });

  zx_time_t Timeout;
  if (!SecondsToWait)
    Timeout = ZX_TIME_INFINITE;
  else
    Timeout = *SecondsToWait * 1000 * 1000 * 1000;

  ProcessInfo WaitResult = PI;
  zx_status_t Status;

  Status = zx_object_wait_one(PI.Process, ZX_PROCESS_TERMINATED, Timeout, nullptr);
  if (Status != ZX_OK) {
    if (Status == ZX_ERR_TIMED_OUT) {
      if (zx_task_kill(PI.Process) != ZX_OK) {
        if (ErrMsg)
          MakeErrMsg(ErrMsg, "Failed to terminate timed-out program");
        WaitResult.ReturnCode = -2;
        return WaitResult;
      }
    } else  {
      MakeErrMsg(ErrMsg, "Error waiting for child process", Status);
      WaitResult.ReturnCode = -1;
      return WaitResult;
    }
  }

  zx_info_process_t Info;
  Status = zx_object_get_info(PI.Process, ZX_INFO_PROCESS, &Info, sizeof(Info),
                              nullptr, nullptr);
  if (Status < 0) {
    if (ErrMsg)
      MakeErrMsg(ErrMsg, "Failed getting status for program");

    WaitResult.ReturnCode = -2;
    return WaitResult;
  }

  WaitResult.ReturnCode = Info.return_code;
  return WaitResult;
}

std::error_code llvm::sys::ChangeStdinMode(fs::OpenFlags Flags) {
  if (!(Flags & fs::OF_Text))
    return ChangeStdinToBinary();
  return std::error_code();
}

std::error_code llvm::sys::ChangeStdoutMode(fs::OpenFlags Flags) {
  if (!(Flags & fs::OF_Text))
    return ChangeStdoutToBinary();
  return std::error_code();
}

std::error_code sys::ChangeStdinToBinary(){
  // Do nothing, as Fuchsia doesn't differentiate between text and binary.
  return std::error_code();
}

std::error_code sys::ChangeStdoutToBinary(){
  // Do nothing, as Fuchsia doesn't differentiate between text and binary.
  return std::error_code();
}

std::error_code
llvm::sys::writeFileWithEncoding(StringRef FileName, StringRef Contents,
                                 WindowsEncodingMethod Encoding /*unused*/) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(FileName, EC,
                          llvm::sys::fs::OpenFlags::OF_TextWithCRLF);

  if (EC)
    return EC;

  OS << Contents;

  if (OS.has_error())
    return make_error_code(errc::io_error);

  return EC;
}

bool sys::commandLineFitsWithinSystemLimits(StringRef Program,
                                            ArrayRef<StringRef> Args) {
  return true;
}

}
