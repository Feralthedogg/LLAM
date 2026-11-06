/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#ifndef LLAM_MANAGED_LLVM_CONTRACT_H
#define LLAM_MANAGED_LLVM_CONTRACT_H

#include "metadata_v2.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <string>

namespace llam {
inline void rejectRetiredBSDTarget(const llvm::Triple &Target) {
    switch (Target.getOS()) {
    case llvm::Triple::FreeBSD:
    case llvm::Triple::OpenBSD:
    case llvm::Triple::NetBSD:
    case llvm::Triple::DragonFly:
        llvm::report_fatal_error("BSD support ended in LLAM 3.0.0; use versioned 2.x sources/tooling.");
    default:
        return;
    }
}

inline constexpr llvm::Attribute::AttrKind ManagedABIAttributes[] = {
    llvm::Attribute::ByVal, llvm::Attribute::ByRef, llvm::Attribute::StructRet,
    llvm::Attribute::InAlloca, llvm::Attribute::Preallocated, llvm::Attribute::InReg,
    llvm::Attribute::SExt, llvm::Attribute::ZExt, llvm::Attribute::NoExt,
    llvm::Attribute::SwiftSelf, llvm::Attribute::SwiftError, llvm::Attribute::SwiftAsync,
    llvm::Attribute::Nest, llvm::Attribute::StackAlignment
};

// Only ABI attributes belong in this ID. Inferred nonnull, readonly and capture
// facts may differ across translation units without changing a C call contract.
inline uint64_t managedTypeIdentity(llvm::FunctionType *Type, llvm::CallingConv::ID CC,
                                   llvm::AttributeList Attributes, const llvm::Triple &T) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Type->print(OS);
    OS << ':' << CC << ':' << (T.isAArch64() ? 2 : 1) << ':'
       << (T.isOSDarwin() ? 2 : (T.isOSWindows() ? 3 : 1));
    for (unsigned I = 0U; I <= Type->getNumParams(); ++I) {
        llvm::AttributeSet Set = I == Type->getNumParams() ? Attributes.getRetAttrs()
                                                        : Attributes.getParamAttrs(I);
        for (llvm::Attribute::AttrKind Kind : ManagedABIAttributes) {
            llvm::Attribute A = Set.getAttribute(Kind);
            if (!A.isValid()) continue;
            OS << ':' << I << ':' << llvm::Attribute::getNameFromAttrKind(Kind);
            // Print full by-value layouts, not only identified struct names.
            // Opaque pointer pointees have no calling-convention representation.
            if (A.isTypeAttribute()) A.getValueAsType()->print(OS, false, false);
            else if (A.isIntAttribute()) OS << '=' << A.getValueAsInt();
        }
        if (Set.hasAttribute(llvm::Attribute::ByVal) || Set.hasAttribute(llvm::Attribute::ByRef)) {
            llvm::Attribute Alignment = Set.getAttribute(llvm::Attribute::Alignment);
            if (Alignment.isValid()) OS << ":align=" << Alignment.getValueAsInt();
        }
    }
    uint64_t Hash = UINT64_C(14695981039346656037);
    for (unsigned char Byte : Text) Hash = (Hash ^ Byte) * UINT64_C(1099511628211);
    return Hash == 0U ? 1U : Hash;
}

inline uint64_t managedTypeIdentity(const llvm::Function &F) {
    return managedTypeIdentity(F.getFunctionType(), F.getCallingConv(), F.getAttributes(),
                               F.getParent()->getTargetTriple());
}

inline bool managedCallMatches(const llvm::CallBase &Call, const llvm::Function &Target) {
    return managedTypeIdentity(Call.getFunctionType(), Call.getCallingConv(), Call.getAttributes(),
                               Target.getParent()->getTargetTriple()) == managedTypeIdentity(Target);
}

inline bool scalarCABI(const llvm::Function &F) {
    if (F.isVarArg() || F.getCallingConv() != llvm::CallingConv::C || F.getAddressSpace() != 0U ||
        F.hasFnAttribute(llvm::Attribute::ReturnsTwice) || F.hasFnAttribute(llvm::Attribute::Naked) ||
        F.hasFnAttribute(llvm::Attribute::Convergent) || F.hasFnAttribute(llvm::Attribute::NoReturn)) return false;
    for (unsigned I = 0U; I <= F.arg_size(); ++I) {
        llvm::AttributeSet Set = I == F.arg_size() ? F.getAttributes().getRetAttrs()
                                                  : F.getAttributes().getParamAttrs(I);
        for (llvm::Attribute::AttrKind Kind : ManagedABIAttributes) {
            if (Set.hasAttribute(Kind)) return false;
        }
    }
    return true;
}

struct RuntimeContract {
    uint64_t ID;
    llvm::StringLiteral Name;
    llvm::StringLiteral Signature;
    unsigned Effect;
};

inline constexpr RuntimeContract RuntimeContracts[] = {
#define LLAM_RUNTIME_CONTRACT(ID, Name, Signature, Effect) {ID, #Name, Signature, LLAM_LLPM2_EFFECT_##Effect},
#include "managed_runtime_contracts.def"
#undef LLAM_RUNTIME_CONTRACT
};

inline llvm::Type *contractType(char Code, llvm::LLVMContext &C, const llvm::Triple &T) {
    switch (Code) {
    case 'v': return llvm::Type::getVoidTy(C);
    case 'p': return llvm::PointerType::getUnqual(C);
    case 'i': return llvm::Type::getInt32Ty(C);
    case 'q': return llvm::Type::getInt64Ty(C);
    case 'f': return T.isOSWindows() ? llvm::Type::getInt64Ty(C) : llvm::Type::getInt32Ty(C);
    case 'h': return T.isOSWindows() ? static_cast<llvm::Type *>(llvm::PointerType::getUnqual(C))
                                    : llvm::Type::getInt32Ty(C);
    default: return nullptr;
    }
}

inline bool matchesContract(const llvm::Function &F, llvm::StringRef Signature) {
    if (!scalarCABI(F) || Signature.size() != F.arg_size() + 1U) return false;
    const llvm::Triple &T = F.getParent()->getTargetTriple();
    if (F.getReturnType() != contractType(Signature.front(), F.getContext(), T)) return false;
    for (unsigned I = 0U; I < F.arg_size(); ++I) {
        if (F.getFunctionType()->getParamType(I) != contractType(Signature[I + 1U], F.getContext(), T)) return false;
    }
    return true;
}

inline const RuntimeContract *runtimeContract(const llvm::Function &F) {
    for (const RuntimeContract &Contract : RuntimeContracts) {
        if (F.getName() == Contract.Name) {
            return matchesContract(F, Contract.Signature) ? &Contract : nullptr;
        }
    }
    return nullptr;
}
} // namespace llam
#endif /* LLAM_MANAGED_LLVM_CONTRACT_H */
