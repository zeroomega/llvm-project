//===- Fuchsia/Memory.cpp - Fuchsia System Configuration --------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines some functions for various memory management utilities.
//
//===----------------------------------------------------------------------===//

#include "Fuchsia.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Process.h"

#include <zircon/process.h>
#include <zircon/syscalls.h>

namespace {

int getZirconProtectionFlags(unsigned Flags) {
  switch (Flags) {
  case llvm::sys::Memory::MF_READ:
    return ZX_VM_FLAG_PERM_READ;
  case llvm::sys::Memory::MF_WRITE:
    return ZX_VM_FLAG_PERM_WRITE;
  case llvm::sys::Memory::MF_READ|llvm::sys::Memory::MF_WRITE:
    return ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_WRITE;
  case llvm::sys::Memory::MF_READ|llvm::sys::Memory::MF_EXEC:
    return ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_EXECUTE;
  case llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE |
      llvm::sys::Memory::MF_EXEC:
    return ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_WRITE |
        ZX_VM_FLAG_PERM_EXECUTE;
  case llvm::sys::Memory::MF_EXEC:
    return ZX_VM_FLAG_PERM_EXECUTE;
  default:
    llvm_unreachable("Illegal memory protection flag specified!");
  }
  // Provide a default return value as required by some compilers.
  return 0;
}

} // anonymous namespace

namespace llvm {
namespace sys {

MemoryBlock
Memory::allocateMappedMemory(size_t NumBytes,
                             const MemoryBlock *const NearBlock,
                             unsigned PFlags,
                             std::error_code &EC) {
  EC = std::error_code();
  if (NumBytes == 0)
    return MemoryBlock();

  int Protect = getZirconProtectionFlags(PFlags);

  // Use any near hint and the page size to set a page-aligned starting address
  uintptr_t Start = NearBlock ? reinterpret_cast<uintptr_t>(NearBlock->base()) +
                                      NearBlock->allocatedSize() : 0;
  static const size_t PageSize = Process::getPageSizeEstimate();
  const size_t NumPages = (NumBytes+PageSize-1)/PageSize;
  const size_t MappingSize = PageSize*NumPages;

  if (Start && Start % PageSize)
    Start += PageSize - Start % PageSize;

  zx_handle_t VMO;
  zx_status_t status = _zx_vmo_create(MappingSize, 0, &VMO);
  if (status != ZX_OK) {
    EC = std::error_code(status, std::generic_category());
    return MemoryBlock();
  }

  uintptr_t Addr;
  status = zx_vmar_map(
      _zx_vmar_root_self(), Protect, 0, VMO, 0, MappingSize, &Addr);
  if (status != ZX_OK) {
    if (NearBlock) //Try again without a near hint
      return allocateMappedMemory(NumBytes, nullptr, PFlags, EC);

    EC = std::error_code(status, std::generic_category());
    return MemoryBlock();
  }

  MemoryBlock Result;
  Result.Address = reinterpret_cast<void *>(Addr);
  Result.AllocatedSize = MappingSize;
  Result.Flags = PFlags;

  // Rely on protectMappedMemory to invalidate instruction cache.
  if (PFlags & MF_EXEC) {
    EC = Memory::protectMappedMemory (Result, PFlags);
    if (EC != std::error_code())
      return MemoryBlock();
  }

  return Result;
}

std::error_code
Memory::releaseMappedMemory(MemoryBlock &M) {
  if (M.Address == nullptr || M.AllocatedSize == 0)
    return std::error_code();

  zx_status_t status = zx_vmar_unmap(
      _zx_vmar_root_self(), reinterpret_cast<uintptr_t>(M.Address), M.AllocatedSize);
  if (status != ZX_OK)
    return std::error_code(status, std::generic_category());

  M.Address = nullptr;
  M.AllocatedSize = 0;

  return std::error_code();
}

std::error_code
Memory::protectMappedMemory(const MemoryBlock &M, unsigned Flags) {
  static const Align PageSize = Align(Process::getPageSizeEstimate());
  if (M.Address == nullptr || M.AllocatedSize == 0)
    return std::error_code();

  if (!Flags)
    return std::error_code(EINVAL, std::generic_category());

  int Protect = getZirconProtectionFlags(Flags);
  uintptr_t Start = alignAddr((const uint8_t *)M.Address - PageSize.value() + 1, PageSize);
  uintptr_t End = alignAddr((const uint8_t *)M.Address + M.AllocatedSize, PageSize);

  bool InvalidateCache = (Flags & MF_EXEC);

#if defined(__arm__) || defined(__aarch64__)
  // Certain ARM implementations treat icache clear instruction as a memory read,
  // and CPU segfaults on trying to clear cache on !PROT_READ page.  Therefore we need
  // to temporarily add PROT_READ for the sake of flushing the instruction caches.
  if (InvalidateCache && !(Protect & PROT_READ)) {
    Memory::InvalidateInstructionCache(M.Address, M.AllocatedSize);
    InvalidateCache = false;
  }
#endif

  zx_status_t status = zx_vmar_protect(
      _zx_vmar_root_self(), Protect, (uintptr_t)Start, End - Start);
  if (status != ZX_OK)
    return std::error_code(status, std::generic_category());

  if (Flags & MF_EXEC)
    Memory::InvalidateInstructionCache(M.Address, M.AllocatedSize);

  return std::error_code();
}

/// InvalidateInstructionCache - Before the JIT can run a block of code
/// that has been emitted it must invalidate the instruction cache on some
/// platforms.
void Memory::InvalidateInstructionCache(const void *Addr,
                                        size_t Len) {
  zx_status_t Status = zx_cache_flush(Addr, Len, ZX_CACHE_FLUSH_INSN);
  assert(Status == ZX_OK && "cannot invalidate instruction cache");
}

} // namespace sys
} // namespace llvm
