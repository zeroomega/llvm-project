//===- Fuchsia/DynamicLibrary.cpp - Fuchsia DL Implementation ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the Fuchsia specific implementation of DynamicLibrary.
//
//===----------------------------------------------------------------------===//

#include <dlfcn.h>

DynamicLibrary::HandleSet::~HandleSet() {
  // Close the libraries in reverse order.
  for (void *Handle : llvm::reverse(Handles))
    ::dlclose(Handle);
  if (Process)
    ::dlclose(Process);

  // llvm_shutdown called, Return to default
  DynamicLibrary::SearchOrder = DynamicLibrary::SO_Linker;
}

void *DynamicLibrary::HandleSet::DLOpen(const char *File, std::string *Err) {
  const char *AdjustedFileName = File;
  if (File != nullptr) {
    int i = 0, SeparatorIndex = -1;
    while (File[i] != '\0') {
      if (File[i] == '/')
        SeparatorIndex = i;
      i++;
    }
    SeparatorIndex++;

    AdjustedFileName = File + SeparatorIndex;
  }
  void *Handle = ::dlopen(AdjustedFileName, RTLD_LAZY|RTLD_GLOBAL);
  if (!Handle) {
    if (Err) *Err = ::dlerror();
    return &DynamicLibrary::Invalid;
  }

  return Handle;
}

void DynamicLibrary::HandleSet::DLClose(void *Handle) {
  ::dlclose(Handle);
}

void *DynamicLibrary::HandleSet::DLSym(void *Handle, const char *Symbol) {
  return ::dlsym(Handle, Symbol);
}

// Must declare the symbols in the global namespace.
static void *DoSearch(const char* SymbolName) {
// This macro returns the address of a well-known, explicit symbol
#define EXPLICIT_SYMBOL(SYM)                                                   \
  extern void *SYM;                                                            \
  if (!strcmp(SymbolName, #SYM))                                               \
  return (void *)&SYM

// The stderr/out/in symbols are both macros and global variables because of
// standards requirements. So, we boldly use the EXPLICIT_SYMBOL macro without
// checking for a #define first.
#if defined(__GLIBC__)
  {
    EXPLICIT_SYMBOL(stderr);
    EXPLICIT_SYMBOL(stdout);
    EXPLICIT_SYMBOL(stdin);
  }
#else
  // For everything else, we want to check to make sure the symbol isn't defined
  // as a macro before using EXPLICIT_SYMBOL.
  {
#ifndef stdin
    EXPLICIT_SYMBOL(stdin);
#endif
#ifndef stdout
    EXPLICIT_SYMBOL(stdout);
#endif
#ifndef stderr
    EXPLICIT_SYMBOL(stderr);
#endif
  }
#endif
#undef EXPLICIT_SYMBOL

  return nullptr;
}
