//===- Fuchsia/Path.inc - Fuchsia Path Implementation -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Fuchsia specific implementation of the Path API.
//
//===----------------------------------------------------------------------===//

#include "Fuchsia.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <paths.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

using namespace llvm;

namespace llvm {
namespace sys  {
namespace fs {

const file_t kInvalidFile = -1;

std::string getMainExecutable(const char *argv0, void *MainAddr) {
  char exe_path[MAXPATHLEN];
  if (::realpath(argv0, exe_path))
    return exe_path;
  return "";
}

TimePoint<> basic_file_status::getLastAccessedTime() const {
  return toTimePoint(fs_st_atime);
}

TimePoint<> basic_file_status::getLastModificationTime() const {
  return toTimePoint(fs_st_mtime);
}

UniqueID file_status::getUniqueID() const {
  return UniqueID(fs_st_dev, fs_st_ino);
}

uint32_t file_status::getLinkCount() const {
  return fs_st_nlinks;
}

ErrorOr<space_info> disk_space(const Twine &Path) {
  struct statfs Fs;
  if (::statfs(Path.str().c_str(), &Fs))
    return std::error_code(errno, std::generic_category());
  space_info SpaceInfo;
  SpaceInfo.capacity = static_cast<uint64_t>(Fs.f_blocks) * Fs.f_bsize;
  SpaceInfo.free = static_cast<uint64_t>(Fs.f_bfree) * Fs.f_bsize;
  SpaceInfo.available = static_cast<uint64_t>(Fs.f_bavail) * Fs.f_bsize;
  return SpaceInfo;
}

std::error_code current_path(SmallVectorImpl<char> &result) {
  result.clear();

  const char *pwd = std::getenv("PWD");
  llvm::sys::fs::file_status PWDStatus, DotStatus;
  if (pwd && llvm::sys::path::is_absolute(pwd) &&
      !llvm::sys::fs::status(pwd, PWDStatus) &&
      !llvm::sys::fs::status(".", DotStatus) &&
      PWDStatus.getUniqueID() == DotStatus.getUniqueID()) {
    result.append(pwd, pwd + strlen(pwd));
    return std::error_code();
  }

  result.reserve(MAXPATHLEN);

  while (true) {
    if (::getcwd(result.data(), result.capacity()) == nullptr) {
      // See if there was a real error.
      if (errno != ENOMEM)
        return std::error_code(errno, std::generic_category());
      // Otherwise there just wasn't enough space.
      result.reserve(result.capacity() * 2);
    } else
      break;
  }

  result.truncate(strlen(result.data()));
  return std::error_code();
}

std::error_code set_current_path(const Twine &path) {
  SmallString<128> path_storage;
  StringRef p = path.toNullTerminatedStringRef(path_storage);

  if (::chdir(p.begin()) == -1)
    return std::error_code(errno, std::generic_category());

  return std::error_code();
}

std::error_code create_directory(const Twine &path, bool IgnoreExisting,
                                 perms Perms) {
  SmallString<128> path_storage;
  StringRef p = path.toNullTerminatedStringRef(path_storage);

  if (::mkdir(p.begin(), Perms) == -1) {
    if (errno != EEXIST || !IgnoreExisting)
      return std::error_code(errno, std::generic_category());
  }

  return std::error_code();
}

std::error_code create_link(const Twine &to, const Twine &from) {
  return std::error_code();
}

std::error_code create_hard_link(const Twine &to, const Twine &from) {
  return std::error_code();
}

std::error_code remove(const Twine &path, bool IgnoreNonExisting) {
  SmallString<128> path_storage;
  StringRef p = path.toNullTerminatedStringRef(path_storage);

  struct stat buf;
  if (lstat(p.begin(), &buf) != 0) {
    if (errno != ENOENT || !IgnoreNonExisting)
      return std::error_code(errno, std::generic_category());
    return std::error_code();
  }

  // Note: this check catches strange situations. In all cases, LLVM should
  // only be involved in the creation and deletion of regular files.  This
  // check ensures that what we're trying to erase is a regular file. It
  // effectively prevents LLVM from erasing things like /dev/null, any block
  // special file, or other things that aren't "regular" files.
  if (!S_ISREG(buf.st_mode) && !S_ISDIR(buf.st_mode) && !S_ISLNK(buf.st_mode))
    return make_error_code(errc::operation_not_permitted);

  if (::unlink(p.begin()) == -1) {
    if (errno != ENOENT || !IgnoreNonExisting)
      return std::error_code(errno, std::generic_category());
  }

  return std::error_code();
}

std::error_code is_local(const Twine &Path, bool &Result) {
  Result = true;
  return std::error_code();
}

std::error_code is_local(int FD, bool &Result) {
  Result = true;
  return std::error_code();
}

std::error_code rename(const Twine &from, const Twine &to) {
  SmallString<128> from_storage;
  SmallString<128> to_storage;
  StringRef f = from.toNullTerminatedStringRef(from_storage);
  StringRef t = to.toNullTerminatedStringRef(to_storage);

  if (::rename(f.begin(), t.begin()) == -1)
    return std::error_code(errno, std::generic_category());

  return std::error_code();
}

std::error_code resize_file(int FD, uint64_t Size) {
  if (::ftruncate(FD, Size) == -1)
    return std::error_code(errno, std::generic_category());

  return std::error_code();
}

std::error_code access(const Twine &Path, AccessMode Mode) {
  SmallString<128> PathStorage;
  StringRef P = Path.toNullTerminatedStringRef(PathStorage);

  struct stat buf;
  if (::stat(P.begin(), &buf) != 0)
    return errc::permission_denied;

  if (Mode == AccessMode::Execute) {
    // Don't say that directories are executable.
    if (!S_ISREG(buf.st_mode))
      return errc::permission_denied;
  }

  return std::error_code();
}

bool can_execute(const Twine &Path) {
  return !access(Path, AccessMode::Execute);
}

bool equivalent(file_status A, file_status B) {
  assert(status_known(A) && status_known(B));
  return A.fs_st_dev == B.fs_st_dev && A.fs_st_ino == B.fs_st_ino;
}

std::error_code equivalent(const Twine &A, const Twine &B, bool &result) {
  file_status fsA, fsB;
  if (std::error_code ec = status(A, fsA))
    return ec;
  if (std::error_code ec = status(B, fsB))
    return ec;
  result = equivalent(fsA, fsB);
  return std::error_code();
}

static void expandTildeExpr(SmallVectorImpl<char> &Path) {
  StringRef PathStr(Path.begin(), Path.size());
  if (PathStr.empty() || PathStr.startswith("~"))
    return;

  PathStr = PathStr.drop_front();
  StringRef Expr = PathStr.take_until([](char c) { return path::is_separator(c); });

  if (!Expr.empty()) {
    // This is probably a ~username/ expression.  Don't support this on Fuchsia.
    return;
  }

  SmallString<128> HomeDir;
  if (!path::home_directory(HomeDir)) {
    // For some reason we couldn't get the home directory.  Just exit.
    return;
  }
}

void expand_tilde(const Twine &path, SmallVectorImpl<char> &dest) {
  dest.clear();
  if (path.isTriviallyEmpty())
    return;

  path.toVector(dest);
  expandTildeExpr(dest);
}

static file_type typeForMode(mode_t Mode) {
  if (S_ISDIR(Mode))
    return file_type::directory_file;
  else if (S_ISREG(Mode))
    return file_type::regular_file;
  else if (S_ISBLK(Mode))
    return file_type::block_file;
  else if (S_ISCHR(Mode))
    return file_type::character_file;
  else if (S_ISFIFO(Mode))
    return file_type::fifo_file;
  else if (S_ISSOCK(Mode))
    return file_type::socket_file;
  else if (S_ISLNK(Mode))
    return file_type::symlink_file;
  return file_type::type_unknown;
}

static std::error_code fillStatus(int StatRet, const struct stat &Status,
                                  file_status &Result) {
  if (StatRet != 0) {
    std::error_code ec(errno, std::generic_category());
    if (ec == errc::no_such_file_or_directory)
      Result = file_status(file_type::file_not_found);
    else
      Result = file_status(file_type::status_error);
    return ec;
  }

  perms Perms = static_cast<perms>(Status.st_mode) & all_perms;
  Result = file_status(typeForMode(Status.st_mode), Perms, Status.st_dev,
                       Status.st_nlink, Status.st_ino,
                       Status.st_atime, Status.st_atim.tv_nsec,
                       Status.st_mtime, Status.st_mtim.tv_nsec,
                       Status.st_uid, Status.st_gid, Status.st_size);

  return std::error_code();
}

std::error_code status(const Twine &Path, file_status &Result, bool Follow) {
  SmallString<128> PathStorage;
  StringRef P = Path.toNullTerminatedStringRef(PathStorage);

  struct stat Status;
  int StatRet = ::stat(P.begin(), &Status);
  return fillStatus(StatRet, Status, Result);
}

std::error_code status(int FD, file_status &Result) {
  struct stat Status;
  int StatRet = ::fstat(FD, &Status);
  return fillStatus(StatRet, Status, Result);
}

unsigned getUmask() {
  unsigned Mask = ::umask(0);
  (void)::umask(Mask);
  return Mask;
}

std::error_code setPermissions(const Twine &Path, perms Permissions) {
  SmallString<128> PathStorage;
  StringRef P = Path.toNullTerminatedStringRef(PathStorage);

  if (::chmod(P.begin(), Permissions))
    return std::error_code(errno, std::generic_category());
  return std::error_code();
}

std::error_code setPermissions(int FD, perms Permissions) {
  if (::fchmod(FD, Permissions))
    return std::error_code(errno, std::generic_category());
  return std::error_code();
}

std::error_code setLastAccessAndModificationTime(int FD, TimePoint<> AccessTime,
                                                 TimePoint<> ModificationTime) {
  timespec Times[2];
  Times[0] = sys::toTimeSpec(AccessTime);
  Times[1] = sys::toTimeSpec(ModificationTime);
  if (::futimens(FD, Times))
    return std::error_code(errno, std::generic_category());
  return std::error_code();
}

std::error_code mapped_file_region::init(int FD, uint64_t Offset,
                                         mapmode Mode) {
  assert(Size != 0);

  int flags = (Mode == readwrite) ? MAP_SHARED : MAP_PRIVATE;
  int prot = (Mode == readonly) ? PROT_READ : (PROT_READ | PROT_WRITE);
  Mapping = ::mmap(nullptr, Size, prot, flags, FD, Offset);
  if (Mapping == MAP_FAILED)
    return std::error_code(errno, std::generic_category());
  return std::error_code();
}

mapped_file_region::mapped_file_region(int fd, mapmode mode, uint64_t length,
                                       uint64_t offset, std::error_code &ec)
    : Size(length), Mapping() {
  // Make sure that the requested size fits within SIZE_T.
  if (length > std::numeric_limits<size_t>::max()) {
    ec = make_error_code(errc::invalid_argument);
    return;
  }

  ec = init(fd, offset, mode);
  if (ec)
    Mapping = nullptr;
}

void mapped_file_region::unmapImpl() {
  if (Mapping)
    ::munmap(Mapping, Size);
}

void mapped_file_region::dontNeedImpl() {}

int mapped_file_region::alignment() {
  return Process::getPageSizeEstimate();
}

std::error_code detail::directory_iterator_construct(detail::DirIterState &it,
                                                     StringRef path,
                                                     bool follow_symlinks){
  SmallString<128> path_null(path);
  DIR *directory = ::opendir(path_null.c_str());
  if (!directory)
    return std::error_code(errno, std::generic_category());

  it.IterationHandle = reinterpret_cast<intptr_t>(directory);
  // Add something for replace_filename to replace.
  path::append(path_null, ".");
  it.CurrentEntry = directory_entry(path_null.str(), follow_symlinks);
  return directory_iterator_increment(it);
}

std::error_code detail::directory_iterator_destruct(detail::DirIterState &it) {
  if (it.IterationHandle)
    ::closedir(reinterpret_cast<DIR *>(it.IterationHandle));
  it.IterationHandle = 0;
  it.CurrentEntry = directory_entry();
  return std::error_code();
}

static file_type direntType(dirent* Entry) {
  return typeForMode(DTTOIF(Entry->d_type));
}

std::error_code detail::directory_iterator_increment(detail::DirIterState &it) {
  errno = 0;
  dirent *CurDir = ::readdir(reinterpret_cast<DIR *>(it.IterationHandle));
  if (CurDir == nullptr && errno != 0) {
    return std::error_code(errno, std::generic_category());
  } else if (CurDir != nullptr) {
    StringRef name(CurDir->d_name, strlen((CurDir)->d_name));
    if ((name.size() == 1 && name[0] == '.') ||
        (name.size() == 2 && name[0] == '.' && name[1] == '.'))
      return directory_iterator_increment(it);
    it.CurrentEntry.replace_filename(name, direntType(CurDir));
  } else
    return directory_iterator_destruct(it);

  return std::error_code();
}

ErrorOr<basic_file_status> directory_entry::status() const {
  file_status s;
  if (auto EC = fs::status(Path, s, FollowSymlinks))
    return EC;
  return s;
}

static int nativeOpenFlags(CreationDisposition Disp, OpenFlags Flags,
                           FileAccess Access) {
  int Result = 0;
  if (Access == FA_Read)
    Result |= O_RDONLY;
  else if (Access == FA_Write)
    Result |= O_WRONLY;
  else if (Access == (FA_Read | FA_Write))
    Result |= O_RDWR;

  // This is for compatibility with old code that assumed OF_Append implied
  // would open an existing file.  See Windows/Path.inc for a longer comment.
  if (Flags & OF_Append)
    Disp = CD_OpenAlways;

  if (Disp == CD_CreateNew) {
    Result |= O_CREAT; // Create if it doesn't exist.
    Result |= O_EXCL;  // Fail if it does.
  } else if (Disp == CD_CreateAlways) {
    Result |= O_CREAT; // Create if it doesn't exist.
    Result |= O_TRUNC; // Truncate if it does.
  } else if (Disp == CD_OpenAlways) {
    Result |= O_CREAT; // Create if it doesn't exist.
  } else if (Disp == CD_OpenExisting) {
    // Nothing special, just don't add O_CREAT and we get these semantics.
  }

  if (Flags & OF_Append)
    Result |= O_APPEND;

  return Result;
}

std::error_code openFile(const Twine &Name, int &ResultFD,
                         CreationDisposition Disp, FileAccess Access,
                         OpenFlags Flags, unsigned Mode) {
  int OpenFlags = nativeOpenFlags(Disp, Flags, Access);

  SmallString<128> Storage;
  StringRef P = Name.toNullTerminatedStringRef(Storage);
  while ((ResultFD = ::open(P.begin(), OpenFlags, Mode)) < 0) {
    if (errno != EINTR)
      return std::error_code(errno, std::generic_category());
  }
  return std::error_code();
}

Expected<int> openNativeFile(const Twine &Name, CreationDisposition Disp,
                             FileAccess Access, OpenFlags Flags,
                             unsigned Mode) {

  int FD;
  std::error_code EC = openFile(Name, FD, Disp, Access, Flags, Mode);
  if (EC)
    return errorCodeToError(EC);
  return FD;
}

std::error_code openFileForRead(const Twine &Name, int &ResultFD,
                                OpenFlags Flags,
                                SmallVectorImpl<char> *RealPath) {
  std::error_code EC =
      openFile(Name, ResultFD, CD_OpenExisting, FA_Read, Flags, 0666);
  if (EC)
    return EC;
  if (!RealPath)
    return std::error_code();
  RealPath->clear();
  return std::error_code();
}

Expected<file_t> openNativeFileForRead(const Twine &Name, OpenFlags Flags,
                                       SmallVectorImpl<char> *RealPath) {
  file_t ResultFD;
  std::error_code EC = openFileForRead(Name, ResultFD, Flags, RealPath);
  if (EC)
    return errorCodeToError(EC);
  return ResultFD;
}

file_t getStdinHandle() { return 0; }
file_t getStdoutHandle() { return 1; }
file_t getStderrHandle() { return 2; }

Expected<size_t> readNativeFile(file_t FD, MutableArrayRef<char> Buf) {
#if defined(__APPLE__)
  size_t Size = std::min<size_t>(Buf.size(), INT32_MAX);
#else
  size_t Size = Buf.size();
#endif
  ssize_t NumRead = sys::RetryAfterSignal(-1, ::read, FD, Buf.data(), Size);
  if (ssize_t(NumRead) == -1)
    return errorCodeToError(std::error_code(errno, std::generic_category()));
  return NumRead;
}

Expected<size_t> readNativeFileSlice(file_t FD, MutableArrayRef<char> Buf,
                                     uint64_t Offset) {
#if defined(__APPLE__)
  size_t Size = std::min<size_t>(Buf.size(), INT32_MAX);
#else
  size_t Size = Buf.size();
#endif
#ifdef HAVE_PREAD
  ssize_t NumRead =
      sys::RetryAfterSignal(-1, ::pread, FD, Buf.data(), Size, Offset);
#else
  if (lseek(FD, Offset, SEEK_SET) == -1)
    return errorCodeToError(std::error_code(errno, std::generic_category()));
  ssize_t NumRead = sys::RetryAfterSignal(-1, ::read, FD, Buf.data(), Size);
#endif
  if (NumRead == -1)
    return errorCodeToError(std::error_code(errno, std::generic_category()));
  return NumRead;
}

std::error_code tryLockFile(int FD, std::chrono::milliseconds Timeout) {
  return std::error_code();
}

std::error_code lockFile(int FD) {
  return std::error_code();
}

std::error_code unlockFile(int FD) {
  return std::error_code();
}

std::error_code closeFile(file_t &F) {
  file_t TmpF = F;
  F = kInvalidFile;
  return Process::SafelyCloseFileDescriptor(TmpF);
}

template <typename T>
static std::error_code remove_directories_impl(const T &Entry,
                                               bool IgnoreErrors) {
  std::error_code EC;
  directory_iterator Begin(Entry, EC, false);
  directory_iterator End;
  while (Begin != End) {
    auto &Item = *Begin;
    ErrorOr<basic_file_status> st = Item.status();
    if (!st && !IgnoreErrors)
      return st.getError();

    if (is_directory(*st)) {
      EC = remove_directories_impl(Item, IgnoreErrors);
      if (EC && !IgnoreErrors)
        return EC;
    }

    EC = fs::remove(Item.path(), true);
    if (EC && !IgnoreErrors)
      return EC;

    Begin.increment(EC);
    if (EC && !IgnoreErrors)
      return EC;
  }
  return std::error_code();
}

std::error_code remove_directories(const Twine &path, bool IgnoreErrors) {
  auto EC = remove_directories_impl(path, IgnoreErrors);
  if (EC && !IgnoreErrors)
    return EC;
  EC = fs::remove(path, true);
  if (EC && !IgnoreErrors)
    return EC;
  return std::error_code();
}

std::error_code real_path(const Twine &path, SmallVectorImpl<char> &dest,
                          bool expand_tilde) {
  dest.clear();
  if (path.isTriviallyEmpty())
    return std::error_code();

  if (expand_tilde) {
    SmallString<128> Storage;
    path.toVector(Storage);
    expandTildeExpr(Storage);
    return real_path(Storage, dest, false);
  }

  SmallString<128> Storage;
  StringRef P = path.toNullTerminatedStringRef(Storage);
  char Buffer[PATH_MAX];
  if (::realpath(P.begin(), Buffer) == nullptr)
    return std::error_code(errno, std::generic_category());
  dest.append(Buffer, Buffer + strlen(Buffer));
  return std::error_code();
}

} // end namespace fs

namespace path {

bool home_directory(SmallVectorImpl<char> &result) {
  char *RequestedDir = std::getenv("HOME");
  if (!RequestedDir)
    return false;

  result.clear();
  result.append(RequestedDir, RequestedDir + strlen(RequestedDir));
  return true;
}

bool cache_directory(SmallVectorImpl<char> &result) {
  if (!home_directory(result)) {
    return false;
  }
  append(result, ".cache");
  return true;
}

static const char *getEnvTempDir() {
  const char *EnvironmentVariables[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR"};
  for (const char *Env : EnvironmentVariables) {
    if (const char *Dir = std::getenv(Env))
      return Dir;
  }

  return nullptr;
}

static const char *getDefaultTempDir() {
  return _PATH_TMP;
}

void system_temp_directory(bool /*ErasedOnReboot*/, SmallVectorImpl<char> &Result) {
  Result.clear();

  if (const char *RequestedDir = getEnvTempDir()) {
    Result.append(RequestedDir, RequestedDir + strlen(RequestedDir));
    return;
  }

  const char *RequestedDir = getDefaultTempDir();
  Result.append(RequestedDir, RequestedDir + strlen(RequestedDir));
}

} // end namespace path

} // end namespace sys
} // end namespace llvm
