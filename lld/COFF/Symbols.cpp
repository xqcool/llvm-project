//===- Symbols.cpp --------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Symbols.h"
#include "InputFiles.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "lld/Common/Strings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::object;

using namespace lld::coff;

static_assert(sizeof(SymbolUnion) <= 48,
              "symbols should be optimized for memory usage");

// Returns a symbol name for an error message.
std::string lld::toString(coff::Symbol &B) {
  if (Config->Demangle)
    if (Optional<std::string> S = lld::demangleMSVC(B.getName()))
      return *S;
  return B.getName();
}

namespace lld {
namespace coff {

StringRef Symbol::getName() {
  // COFF symbol names are read lazily for a performance reason.
  // Non-external symbol names are never used by the linker except for logging
  // or debugging. Their internal references are resolved not by name but by
  // symbol index. And because they are not external, no one can refer them by
  // name. Object files contain lots of non-external symbols, and creating
  // StringRefs for them (which involves lots of strlen() on the string table)
  // is a waste of time.
  if (NameData == nullptr) {
    auto *D = cast<DefinedCOFF>(this);
    StringRef NameStr;
    cast<ObjFile>(D->File)->getCOFFObj()->getSymbolName(D->Sym, NameStr);
    NameData = NameStr.data();
    NameSize = NameStr.size();
    assert(NameSize == NameStr.size() && "name length truncated");
  }
  return StringRef(NameData, NameSize);
}

InputFile *Symbol::getFile() {
  if (auto *Sym = dyn_cast<DefinedCOFF>(this))
    return Sym->File;
  if (auto *Sym = dyn_cast<Lazy>(this))
    return Sym->File;
  return nullptr;
}

bool Symbol::isLive() const {
  if (auto *R = dyn_cast<DefinedRegular>(this))
    return R->getChunk()->Live;
  if (auto *Imp = dyn_cast<DefinedImportData>(this))
    return Imp->File->Live;
  if (auto *Imp = dyn_cast<DefinedImportThunk>(this))
    return Imp->WrappedSym->File->ThunkLive;
  // Assume any other kind of symbol is live.
  return true;
}

// MinGW specific.
void Symbol::replaceKeepingName(Symbol *Other, size_t Size) {
  StringRef OrigName = getName();
  memcpy(this, Other, Size);
  NameData = OrigName.data();
  NameSize = OrigName.size();
}

COFFSymbolRef DefinedCOFF::getCOFFSymbol() {
  size_t SymSize = cast<ObjFile>(File)->getCOFFObj()->getSymbolTableEntrySize();
  if (SymSize == sizeof(coff_symbol16))
    return COFFSymbolRef(reinterpret_cast<const coff_symbol16 *>(Sym));
  assert(SymSize == sizeof(coff_symbol32));
  return COFFSymbolRef(reinterpret_cast<const coff_symbol32 *>(Sym));
}

uint16_t DefinedAbsolute::NumOutputSections;

static Chunk *makeImportThunk(DefinedImportData *S, uint16_t Machine) {
  if (Machine == AMD64)
    return make<ImportThunkChunkX64>(S);
  if (Machine == I386)
    return make<ImportThunkChunkX86>(S);
  if (Machine == ARM64)
    return make<ImportThunkChunkARM64>(S);
  assert(Machine == ARMNT);
  return make<ImportThunkChunkARM>(S);
}

DefinedImportThunk::DefinedImportThunk(StringRef Name, DefinedImportData *S,
                                       uint16_t Machine)
    : Defined(DefinedImportThunkKind, Name), WrappedSym(S),
      Data(makeImportThunk(S, Machine)) {}

Defined *Undefined::getWeakAlias() {
  // A weak alias may be a weak alias to another symbol, so check recursively.
  for (Symbol *A = WeakAlias; A; A = cast<Undefined>(A)->WeakAlias)
    if (auto *D = dyn_cast<Defined>(A))
      return D;
  return nullptr;
}
} // namespace coff
} // namespace lld
