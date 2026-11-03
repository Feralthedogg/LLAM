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

#include "metadata_v2.h"
#include "managed_llvm_contract.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/CallPromotionUtils.h"

#include <cstdlib>
#include <functional>
#include <string>

using namespace llvm;

namespace {
constexpr unsigned Managed = 1U << LLAM_LLPM2_EFFECT_MANAGED;
constexpr unsigned Unknown = 1U << LLAM_LLPM2_EFFECT_UNKNOWN;
constexpr unsigned Affine = 1U << LLAM_LLPM2_EFFECT_NATIVE_AFFINE;

using llam::managedTypeIdentity;

bool finiteLocalTargets(Value *Pointer, SmallVectorImpl<Function *> &Targets,
                        SmallPtrSetImpl<Value *> &Active, unsigned Depth = 0U) {
    if (Depth == 8U || !Active.insert(Pointer).second) return false;
    bool Known = true;
    if (auto *Target = dyn_cast<Function>(Pointer)) {
        if (Target->isDeclaration() || !Target->hasLocalLinkage()) Known = false;
        else if (!llvm::is_contained(Targets, Target)) Targets.push_back(Target);
    } else if (auto *Select = dyn_cast<SelectInst>(Pointer)) {
        Known = finiteLocalTargets(Select->getTrueValue(), Targets, Active, Depth + 1U) &&
                finiteLocalTargets(Select->getFalseValue(), Targets, Active, Depth + 1U);
    } else if (auto *Phi = dyn_cast<PHINode>(Pointer)) {
        for (Value *Incoming : Phi->incoming_values()) {
            if (!finiteLocalTargets(Incoming, Targets, Active, Depth + 1U)) { Known = false; break; }
        }
    } else Known = false;
    Active.erase(Pointer);
    return Known && Targets.size() <= 8U;
}

// A closed SSA set needs no runtime resolver. Every possible target becomes a
// direct call and then participates in ordinary transitive effect and final-link
// closure checks. Loads, parameters, aliases and cyclic PHIs stay unqualified.
void lowerFiniteIndirectCalls(Module &M) {
    SmallVector<CallInst *, 16> Calls;
    for (Function &F : M) {
        for (Instruction &I : instructions(F)) {
            auto *Call = dyn_cast<CallInst>(&I);
            if (Call != nullptr && Call->getCalledFunction() == nullptr && !Call->isInlineAsm() &&
                !Call->isMustTailCall()) Calls.push_back(Call);
        }
    }
    for (CallInst *Call : Calls) {
        SmallVector<Function *, 8> Targets;
        SmallPtrSet<Value *, 16> Active;
        if (!finiteLocalTargets(Call->getCalledOperand(), Targets, Active) || Targets.empty()) continue;
        bool Matches = true;
        for (Function *Target : Targets) Matches &= llam::managedCallMatches(*Call, *Target);
        if (!Matches) continue;
        for (size_t I = 0U; I + 1U < Targets.size(); ++I) promoteCallWithIfThenElse(*Call, Targets[I]);
        promoteCall(*Call, Targets.back());
    }
}

void lowerManagedTargetMarkers(Module &M) {
    Function *Marker = M.getFunction("__llam_managed_target");
    if (Marker == nullptr) return;
    if (!Marker->isDeclaration() || !llam::matchesContract(*Marker, "pp")) {
        report_fatal_error("LLAM managed target marker ABI is unqualified");
    }
    SmallVector<CallInst *, 16> Markers;
    for (User *Use : Marker->users()) {
        auto *Call = dyn_cast<CallInst>(Use);
        if (Call == nullptr || Call->getCalledFunction() != Marker ||
            !llam::managedCallMatches(*Call, *Marker) || Call->isMustTailCall()) {
            report_fatal_error("LLAM managed target marker escaped its call contract");
        }
        Markers.push_back(Call);
    }
    DenseMap<Function *, Function *> Declarations;
    for (CallInst *MarkerCall : Markers) {
        auto *Target = dyn_cast<Function>(MarkerCall->getArgOperand(0));
        if (Target == nullptr || Target->isIntrinsic() || Target == Marker || Target->isVarArg() ||
            Target->getCallingConv() != CallingConv::C || Target->hasFnAttribute(Attribute::ReturnsTwice)) {
            report_fatal_error("LLAM managed target marker requires one fixed C function");
        }
        SmallVector<CallBase *, 8> Uses;
        for (User *Use : MarkerCall->users()) {
            auto *Call = dyn_cast<CallInst>(Use);
            if (Call == nullptr || Call->getCalledOperand() != MarkerCall || Call->isMustTailCall() ||
                !llam::managedCallMatches(*Call, *Target)) {
                report_fatal_error("LLAM managed target marker escaped or has an unqualified call ABI");
            }
            Uses.push_back(Call);
        }
        if (Uses.empty()) report_fatal_error("LLAM managed target marker has no checked call");
        Function *Body = Target;
        if (Target->isDeclaration()) {
            Function *&Declaration = Declarations[Target];
            if (Declaration == nullptr) {
                std::string Name = (Target->getName() + ".llam.body").str();
                if (M.getNamedValue(Name) != nullptr) report_fatal_error("LLAM managed body symbol is reserved");
                Declaration = Function::Create(Target->getFunctionType(), GlobalValue::ExternalLinkage,
                    Target->getAddressSpace(), Name, &M);
                Declaration->setCallingConv(Target->getCallingConv());
                Declaration->setAttributes(Target->getAttributes());
                Declaration->setVisibility(GlobalValue::HiddenVisibility);
                Declaration->addFnAttr("llam.managed.declaration");
                Declaration->addFnAttr("llam.managed.class", "1");
                Declaration->addFnAttr("llam.managed.native", Target->getName());
                if (M.getTargetTriple().isOSBinFormatELF()) {
                    Declaration->addFnAttr("llam.managed.identity", ("__llam_native_identity." + Target->getName()).str());
                }
                Declaration->addFnAttr("llam.managed.type", std::to_string(managedTypeIdentity(*Target)));
            }
            Body = Declaration;
        }
        for (CallBase *Call : Uses) Call->setCalledFunction(Body);
        MarkerCall->eraseFromParent();
    }
}

DenseMap<const Function *, std::string> annotations(Module &M) {
    DenseMap<const Function *, std::string> Result;
    const GlobalVariable *Global = M.getGlobalVariable("llvm.global.annotations");
    if (Global == nullptr || !Global->hasInitializer()) return Result;
    const auto *Array = dyn_cast<ConstantArray>(Global->getInitializer());
    if (Array == nullptr) return Result;
    for (const Use &Use : Array->operands()) {
        const auto *Record = dyn_cast<ConstantStruct>(Use.get());
        if (Record == nullptr || Record->getNumOperands() < 2U) continue;
        const auto *F = dyn_cast<Function>(Record->getOperand(0)->stripPointerCasts());
        const auto *String = dyn_cast<GlobalVariable>(Record->getOperand(1)->stripPointerCasts());
        if (F == nullptr || String == nullptr || !String->hasInitializer()) continue;
        const auto *Data = dyn_cast<ConstantDataArray>(String->getInitializer());
        if (Data != nullptr && Data->isCString()) {
            Result[F] += Data->getAsCString().str() + ";";
        }
    }
    return Result;
}

bool referencesTLS(const Value *V, unsigned Depth = 0U) {
    if (const auto *Global = dyn_cast<GlobalVariable>(V)) return Global->isThreadLocal();
    if (Depth == 8U) return isa<ConstantExpr>(V); // Unresolved constant graphs fail closed.
    if (const auto *Expr = dyn_cast<ConstantExpr>(V)) {
        for (const Use &Operand : Expr->operands()) {
            if (referencesTLS(Operand.get(), Depth + 1U)) return true;
        }
    }
    return false;
}

unsigned declaredEffect(const Function &F,
                        const DenseMap<const Function *, std::string> &Annotations,
                        const TargetLibraryInfo &TLI) {
    if (F.isVarArg() || F.getCallingConv() != CallingConv::C || F.getAddressSpace() != 0U ||
        F.hasFnAttribute(Attribute::ReturnsTwice) || F.hasFnAttribute(Attribute::Naked) ||
        F.hasFnAttribute(Attribute::Convergent)) return LLAM_LLPM2_EFFECT_UNKNOWN;
    auto It = Annotations.find(&F);
    StringRef Annotation = It == Annotations.end() ? StringRef() : StringRef(It->second);
    if (Annotation.contains("llam-foreign-fast;")) return LLAM_LLPM2_EFFECT_FOREIGN_FAST;
    if (Annotation.contains("llam-foreign-blocking;")) return LLAM_LLPM2_EFFECT_FOREIGN_BLOCKING;
    if (Annotation.contains("llam-native-affine;")) return LLAM_LLPM2_EFFECT_NATIVE_AFFINE;
    if (const llam::RuntimeContract *Contract = llam::runtimeContract(F)) return Contract->Effect;
    // The SDK contract and LLVM's target libc model must agree on both the
    // symbol and its ABI. A familiar spelling alone never grants an effect.
    LibFunc LibraryFunction;
    if (TLI.getLibFunc(F, LibraryFunction) && TLI.has(LibraryFunction)) {
        StringRef Signature;
        switch (LibraryFunction) {
        case LibFunc_memcpy: case LibFunc_memmove: Signature = "pppq"; break;
        case LibFunc_memset: Signature = "ppiq"; break;
        case LibFunc_memcmp: Signature = "ippq"; break;
        case LibFunc_malloc: Signature = "pq"; break;
        case LibFunc_calloc: Signature = "pqq"; break;
        case LibFunc_realloc: Signature = "ppq"; break;
        case LibFunc_free: Signature = "vp"; break;
        default: break;
        }
        if (!Signature.empty() && llam::matchesContract(F, Signature)) return LLAM_LLPM2_EFFECT_FOREIGN_FAST;
    }
    const Triple &T = F.getParent()->getTargetTriple();
    if (llam::matchesContract(F, "p") &&
        ((F.getName() == "__error" && (T.isOSDarwin() || T.isOSFreeBSD())) ||
         (F.getName() == "__errno_location" && T.isOSLinux()) ||
         (F.getName() == "_errno" && T.isOSWindows()))) return LLAM_LLPM2_EFFECT_NATIVE_AFFINE;
    return LLAM_LLPM2_EFFECT_UNKNOWN;
}

// LLVM may lower these ordinary C operations to libc after the IR pass. Lower
// nonvolatile memory intrinsics now so the same typed native boundary protects
// both inlined and out-of-line implementations. Volatile/address-space variants
// retain UNKNOWN rather than silently changing their memory semantics.
void lowerMemoryIntrinsics(Module &M) {
    SmallVector<MemIntrinsic *, 16> Memory;
    for (Function &F : M) {
        for (Instruction &I : instructions(F)) {
            auto *MI = dyn_cast<MemIntrinsic>(&I);
            if (MI != nullptr && (MI->getIntrinsicID() == Intrinsic::memcpy ||
                    MI->getIntrinsicID() == Intrinsic::memmove || MI->getIntrinsicID() == Intrinsic::memset)) {
                Memory.push_back(MI);
            }
        }
    }
    LLVMContext &C = M.getContext();
    Type *Pointer = PointerType::getUnqual(C);
    Type *Size = Type::getInt64Ty(C);
    for (MemIntrinsic *MI : Memory) {
        if (MI->isVolatile() || MI->getDestAddressSpace() != 0U ||
            MI->getLength()->getType()->getIntegerBitWidth() > 64U) continue;
        auto *Transfer = dyn_cast<MemTransferInst>(MI);
        if (Transfer != nullptr && Transfer->getSourceAddressSpace() != 0U) continue;
        bool Set = isa<MemSetInst>(MI);
        StringRef Name = Set ? "memset" : (isa<MemMoveInst>(MI) ? "memmove" : "memcpy");
        FunctionType *Type = FunctionType::get(Pointer, {Pointer, Set ? Type::getInt32Ty(C) : Pointer, Size}, false);
        Function *Existing = M.getFunction(Name);
        if (Existing != nullptr && (!Existing->isDeclaration() || Existing->getFunctionType() != Type ||
                                    !llam::scalarCABI(*Existing))) continue;
        FunctionCallee Target = M.getOrInsertFunction(Name, Type);
        IRBuilder<> B(MI);
        if (const auto *Length = dyn_cast<ConstantInt>(MI->getLength())) {
            if (Length->isZero()) {
                MI->eraseFromParent();
                continue;
            }
        } else {
            // LLVM's zero-length intrinsic accepts even null/poison pointers;
            // preserve that no-access case without calling a C library routine.
            Value *Nonempty = B.CreateICmpNE(MI->getLength(), ConstantInt::get(MI->getLength()->getType(), 0));
            B.SetInsertPoint(SplitBlockAndInsertIfThen(Nonempty, MI, false));
        }
        Value *Input = Set ? B.CreateZExt(cast<MemSetInst>(MI)->getValue(), B.getInt32Ty())
                          : Transfer->getRawSource();
        CallInst *Call = B.CreateCall(Target, {MI->getRawDest(), Input, B.CreateZExtOrTrunc(MI->getLength(), Size)});
        Call->setDebugLoc(MI->getDebugLoc());
        MI->eraseFromParent();
    }
}

unsigned localEffects(Function &F,
                      const DenseMap<const Function *, std::string> &Annotations,
                      const TargetLibraryInfo &TLI) {
    unsigned Effects = Managed;
    if (F.isVarArg() || F.getCallingConv() != CallingConv::C || F.hasPersonalityFn() ||
        F.hasFnAttribute(Attribute::Naked) || F.hasFnAttribute(Attribute::ReturnsTwice) ||
        F.hasFnAttribute(Attribute::Convergent)) Effects |= Unknown;
    auto It = Annotations.find(&F);
    if (It != Annotations.end()) {
        StringRef A(It->second);
        if (A.contains("llam-legacy;")) Effects |= Unknown;
        if (A.contains("llam-native-affine;")) Effects |= Affine;
        if (A.contains("llam-no-preempt;")) Effects |= Unknown;
    }
    for (Instruction &I : instructions(F)) {
        if (isa<InvokeInst>(I) || isa<CallBrInst>(I) || isa<IndirectBrInst>(I) ||
            isa<ResumeInst>(I) || I.isEHPad()) Effects |= Unknown;
        if (const auto *A = dyn_cast<AllocaInst>(&I)) {
            if (!A->isStaticAlloca()) Effects |= Unknown;
        }
        for (const Use &Operand : I.operands()) {
            if (referencesTLS(Operand.get())) Effects |= Affine;
        }
        const auto *Call = dyn_cast<CallBase>(&I);
        if (Call == nullptr) continue;
        if (Call->isInlineAsm() || Call->isMustTailCall()) {
            Effects |= Unknown;
            continue;
        }
        const Function *Callee = Call->getCalledFunction();
        if (Callee == nullptr) {
            // Unannotated native function pointers do not acquire managed effects
            // merely because their C signatures happen to match.
            Effects |= Unknown;
        } else if (!llam::managedCallMatches(*Call, *Callee)) {
            Effects |= Unknown;
        } else if (Callee->isIntrinsic()) {
            StringRef Name = Callee->getName();
            if (!(Name.starts_with("llvm.dbg.") || Name.starts_with("llvm.lifetime.") ||
                  Name == "llvm.assume" || Name.starts_with("llvm.fmuladd.") ||
                  Name.starts_with("llvm.threadlocal.address.") ||
                  Name.starts_with("llvm.fma.") || Name.starts_with("llvm.sqrt.") ||
                  Name.starts_with("llvm.smax.") || Name.starts_with("llvm.smin.") ||
                  Name.starts_with("llvm.umax.") || Name.starts_with("llvm.umin.") ||
                  Name.starts_with("llvm.uadd.sat.") || Name.starts_with("llvm.usub.sat.") ||
                  Name.starts_with("llvm.sadd.sat.") || Name.starts_with("llvm.ssub.sat.") ||
                  Name.starts_with("llvm.vector.reduce.") ||
                  Name.starts_with("llvm.abs.") || Name.starts_with("llvm.ctpop.") ||
                  Name.starts_with("llvm.ctlz.") || Name.starts_with("llvm.cttz."))) {
                Effects |= Unknown;
            }
        } else if (Callee->isDeclaration()) {
            unsigned Effect = Callee->hasFnAttribute("llam.managed.declaration")
                ? LLAM_LLPM2_EFFECT_MANAGED : declaredEffect(*Callee, Annotations, TLI);
            // Typed blocking adapters are emitted by the SDK; an annotation on
            // an arbitrary native declaration cannot make an inline call park.
            if (Effect == LLAM_LLPM2_EFFECT_FOREIGN_BLOCKING) Effects |= Unknown;
            else Effects |= 1U << Effect;
        }
    }
    return Effects;
}

class ManagedPass : public PassInfoMixin<ManagedPass> {
public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        const bool Required = std::getenv("LLAM_MANAGED_REQUIRED") != nullptr;
        lowerManagedTargetMarkers(M);
        lowerFiniteIndirectCalls(M);
        lowerMemoryIntrinsics(M);
        TargetLibraryInfoImpl LibraryModel(M.getTargetTriple());
        TargetLibraryInfo TLI(LibraryModel);
        auto Annotations = annotations(M);
        SmallVector<Function *, 32> Functions;
        DenseMap<Function *, unsigned> Effects;
        MD5 Digest;
        std::string IR;
        raw_string_ostream IRStream(IR);
        M.print(IRStream, nullptr);
        Digest.update(IR);
        MD5::MD5Result Sum;
        Digest.final(Sum);
        SmallString<32> BuildId;
        MD5::stringifyResult(Sum, BuildId);
        for (Function &F : M) {
            if (F.isDeclaration() || F.getName().starts_with("__llam_") ||
                F.getName() == "llam_managed_native_enter" || F.getName() == "llam_managed_native_leave") continue;
            // LLVM may choose fastcc for a nonescaping C helper before this
            // final IR pass. Normalize that private ABI and all of its direct
            // callers together; no native function-pointer contract is changed.
            if (F.hasLocalLinkage() && F.getCallingConv() == CallingConv::Fast && !F.hasAddressTaken()) {
                F.setCallingConv(CallingConv::C);
                for (User *U : F.users()) {
                    if (auto *Call = dyn_cast<CallBase>(U)) Call->setCallingConv(CallingConv::C);
                }
            }
            Functions.push_back(&F);
            Effects[&F] = localEffects(F, Annotations, TLI);
        }
        bool Changed;
        do {
            Changed = false;
            for (Function *F : Functions) {
                unsigned E = Effects[F];
                for (Instruction &I : instructions(F)) {
                    const auto *Call = dyn_cast<CallBase>(&I);
                    if (Call == nullptr) continue;
                    Function *Callee = Call->getCalledFunction();
                    auto It = Effects.find(Callee);
                    if (It != Effects.end()) E |= It->second;
                }
                Changed |= E != Effects[F];
                Effects[F] = E;
            }
        } while (Changed);

        LLVMContext &C = M.getContext();
        for (StringRef Name : {"llam_managed_native_enter", "llam_managed_native_leave"}) {
            const Function *Guard = M.getFunction(Name);
            if (Guard != nullptr && !llam::matchesContract(*Guard, "v")) {
                report_fatal_error(Twine("LLAM native guard ABI is unqualified: ") + Name);
            }
        }
        FunctionCallee Enter = M.getOrInsertFunction("llam_managed_native_enter",
                                                    FunctionType::get(Type::getVoidTy(C), false));
        FunctionCallee Leave = M.getOrInsertFunction("llam_managed_native_leave",
                                                    FunctionType::get(Type::getVoidTy(C), false));
        bool HasManaged = false;
        for (Function *F : Functions) HasManaged |= (Effects[F] & Unknown) == 0U;
        StructType *BindingType = StructType::get(C, {PointerType::getUnqual(C), PointerType::getUnqual(C)});
        GlobalVariable *Bindings = nullptr;
        if (HasManaged) {
            Constant *Initializer = ConstantStruct::get(BindingType,
                {cast<Constant>(Enter.getCallee()), cast<Constant>(Leave.getCallee())});
            Bindings = new GlobalVariable(M, BindingType, true, GlobalValue::InternalLinkage,
                Initializer, "__llam_guard_bindings_" + BuildId);
            Bindings->setExternallyInitialized(true);
            Bindings->setAlignment(Align(8));
            appendToCompilerUsed(M, {Bindings});
        }
        auto EmitGuard = [&](IRBuilder<> &B, unsigned Index) {
            if (Bindings == nullptr) report_fatal_error("LLAM missing native guard bindings");
            Value *Slot = B.CreateStructGEP(BindingType, Bindings, Index);
            LoadInst *Pointer = B.CreateLoad(PointerType::getUnqual(C), Slot);
            Pointer->setVolatile(true);
            B.CreateCall(FunctionType::get(Type::getVoidTy(C), false), Pointer);
        };
        DenseMap<Function *, Function *> Bodies;
        for (Function *Body : Functions) {
            const unsigned E = Effects[Body];
            const bool HadAddressTaken = Body->hasAddressTaken();
            auto A = Annotations.find(Body);
            bool ExplicitLegacy = A != Annotations.end() &&
                StringRef(A->second).contains("llam-legacy;");
            if ((E & Unknown) != 0U && Required && !ExplicitLegacy) {
                report_fatal_error(Twine("LLAM managed effect/ABI is unqualified: ") + Body->getName());
            }
            Body->addFnAttr("llam.managed.class", (E & Unknown) != 0U ? "0" : ((E & Affine) != 0U ? "2" : "1"));
            Body->addFnAttr("llam.managed.effects", std::to_string(E));
            Body->addFnAttr("llam.managed.type", std::to_string(managedTypeIdentity(*Body)));
            Body->addFnAttr("llam.managed.build", BuildId);
            if (M.getTargetTriple().isOSBinFormatELF()) {
                // ELF cannot encode read-only PREL64 metadata relocations to
                // preemptible exported symbols. A hidden same-address identity
                // anchors this definition without changing its native export or
                // the ordinary linker's interposition rules.
                bool SharedIdentity = Body->getLinkage() == GlobalValue::ExternalLinkage;
                std::string IdentityName = ("__llam_native_identity." + Body->getName()).str();
                if (!SharedIdentity) IdentityName += "." + BuildId.str().str();
                if (M.getNamedValue(IdentityName) != nullptr) report_fatal_error("LLAM native identity symbol is reserved");
                GlobalAlias *Identity = GlobalAlias::create(Body->getValueType(), Body->getAddressSpace(),
                    SharedIdentity ? GlobalValue::ExternalLinkage : GlobalValue::InternalLinkage,
                    IdentityName, Body, &M);
                if (SharedIdentity) Identity->setVisibility(GlobalValue::HiddenVisibility);
                Body->addFnAttr("llam.managed.identity", IdentityName);
                appendToCompilerUsed(M, {Identity});
            }
            if (Bindings != nullptr) Body->addFnAttr("llam.managed.bindings", Bindings->getName());
            if ((E & Unknown) != 0U) {
                Body->addFnAttr("llam.managed.native", Body->getName());
                continue;
            }
            if (Body->hasLocalLinkage() && !HadAddressTaken) {
                // Nonescaping helpers have no native entry contract. Keep one
                // body instead of retaining an otherwise dead guard thunk.
                Body->addFnAttr("llam.managed.flags", "1");
                Body->addFnAttr("llam.managed.native", Body->getName());
                Body->addFnAttr(Attribute::NoInline);
                continue;
            }
            std::string Name = Body->getName().str();
            Body->setName(Name + ".llam.body");
            Function *Native = Function::Create(Body->getFunctionType(), Body->getLinkage(),
                                                Body->getAddressSpace(), Name, &M);
            Native->setCallingConv(Body->getCallingConv());
            Native->setVisibility(Body->getVisibility());
            Native->setDLLStorageClass(Body->getDLLStorageClass());
            Native->setAttributes(Body->getAttributes());
            // Native wrappers have observable domain transitions even when the
            // original function was inferred pure, nonrecursive or nonblocking.
            Native->setMemoryEffects(MemoryEffects::unknown());
            for (Attribute::AttrKind Kind : {Attribute::NoFree, Attribute::NoSync,
                    Attribute::WillReturn, Attribute::Speculatable, Attribute::NoRecurse,
                    Attribute::MustProgress, Attribute::NoCallback}) Native->removeFnAttr(Kind);
            Native->removeFnAttr("llam.managed.class");
            Native->removeFnAttr("llam.managed.effects");
            Native->removeFnAttr("llam.managed.type");
            Native->removeFnAttr("llam.managed.build");
            Native->addFnAttr(Attribute::NoInline);
            appendToCompilerUsed(M, {Native});
            Body->replaceAllUsesWith(Native);
            bool CrossTranslationUnit = Body->getLinkage() == GlobalValue::ExternalLinkage;
            Body->setLinkage(CrossTranslationUnit ? GlobalValue::ExternalLinkage : GlobalValue::InternalLinkage);
            Body->setVisibility(CrossTranslationUnit ? GlobalValue::HiddenVisibility : GlobalValue::DefaultVisibility);
            Body->setDLLStorageClass(GlobalValue::DefaultStorageClass);
            Body->setComdat(nullptr);
            Body->setUnnamedAddr(GlobalValue::UnnamedAddr::None);
            Body->addFnAttr(Attribute::NoInline);
            Body->addFnAttr("llam.managed.native", Name);
            Bodies[Native] = Body;
        }
        for (const auto &[Native, Body] : Bodies) {
            IRBuilder<> B(BasicBlock::Create(C, "native.entry", Native));
            EmitGuard(B, 0U);
            SmallVector<Value *, 8> Args;
            for (Argument &Arg : Native->args()) Args.push_back(&Arg);
            CallInst *Call = B.CreateCall(Body, Args);
            Call->setCallingConv(Body->getCallingConv());
            Call->setAttributes(Body->getAttributes());
            Call->setTailCallKind(CallInst::TCK_NoTail);
            if (Body->hasFnAttribute(Attribute::NoReturn)) {
                B.CreateUnreachable();
                continue;
            }
            EmitGuard(B, 1U);
            if (Native->getReturnType()->isVoidTy()) B.CreateRetVoid();
            else B.CreateRet(Call);
        }
        for (Function *F : Functions) {
            if ((Effects[F] & Unknown) != 0U) continue;
            for (Instruction &I : instructions(F)) {
                auto *Call = dyn_cast<CallBase>(&I);
                if (Call == nullptr) continue;
                auto It = Bodies.find(Call->getCalledFunction());
                if (It != Bodies.end()) Call->setCalledFunction(It->second);
                // Retain an actual return boundary for native domain and P
                // ownership validation. Late codegen must not form tail calls.
                if (auto *CI = dyn_cast<CallInst>(Call)) CI->setTailCallKind(CallInst::TCK_NoTail);
            }
        }
        DenseMap<Function *, Function *> ForeignBridges;
        for (Function *F : Functions) {
            if ((Effects[F] & Unknown) != 0U) continue;
            SmallVector<CallInst *, 8> ForeignCalls;
            for (Instruction &I : instructions(F)) {
                auto *Call = dyn_cast<CallInst>(&I);
                if (Call == nullptr) continue;
                Function *Target = Call->getCalledFunction();
                if (Target != nullptr && Target->isDeclaration() && !Target->isIntrinsic() &&
                    !Target->hasFnAttribute("llam.managed.declaration")) {
                    ForeignCalls.push_back(Call);
                }
            }
            for (CallInst *Call : ForeignCalls) {
                Function *Target = Call->getCalledFunction();
                unsigned Effect = declaredEffect(*Target, Annotations, TLI);
                Function *&Bridge = ForeignBridges[Target];
                if (Bridge == nullptr) {
                    Bridge = Function::Create(Target->getFunctionType(), GlobalValue::InternalLinkage,
                        Target->getAddressSpace(), "__llam.bridge." + Target->getName(), &M);
                    Bridge->setCallingConv(Target->getCallingConv());
                    Bridge->setAttributes(Target->getAttributes());
                    Bridge->setMemoryEffects(MemoryEffects::unknown());
                    for (Attribute::AttrKind Kind : {Attribute::NoFree, Attribute::NoSync,
                            Attribute::WillReturn, Attribute::Speculatable, Attribute::NoRecurse,
                            Attribute::MustProgress, Attribute::NoCallback}) Bridge->removeFnAttr(Kind);
                    Bridge->addFnAttr(Attribute::NoInline);
                    Bridge->addFnAttr("llam.foreign.effect", std::to_string(Effect));
                    Bridge->addFnAttr("llam.managed.class", "0");
                    bool NativeDomain = llam::runtimeContract(*Target) == nullptr;
                    Bridge->addFnAttr("llam.managed.flags", NativeDomain ? "9" : "17");
                    Bridge->addFnAttr("llam.managed.effects", std::to_string(1U << Effect));
                    Bridge->addFnAttr("llam.managed.type", std::to_string(managedTypeIdentity(*Target)));
                    Bridge->addFnAttr("llam.managed.build", BuildId);
                    Bridge->addFnAttr("llam.managed.native", Bridge->getName());
                    if (NativeDomain) Bridge->addFnAttr("llam.managed.bindings", Bindings->getName());
                    IRBuilder<> B(BasicBlock::Create(C, "foreign.entry", Bridge));
                    if (NativeDomain) EmitGuard(B, 0U);
                    SmallVector<Value *, 8> Args;
                    for (Argument &Arg : Bridge->args()) Args.push_back(&Arg);
                    Value *CallTarget = Target;
                    if (!NativeDomain) {
                        const llam::RuntimeContract *Contract = llam::runtimeContract(*Target);
                        if (Contract == nullptr) report_fatal_error("LLAM runtime bridge lacks an exact SDK contract");
                        auto *Slot = new GlobalVariable(M, PointerType::getUnqual(C), true,
                            GlobalValue::InternalLinkage, Target,
                            "__llam_runtime_binding_" + std::to_string(Contract->ID) + "_" + BuildId);
                        Slot->setExternallyInitialized(true);
                        Slot->setAlignment(Align(8));
                        appendToCompilerUsed(M, {Slot});
                        Bridge->addFnAttr("llam.runtime.binding", Slot->getName());
                        Bridge->addFnAttr("llam.runtime.contract", std::to_string(Contract->ID));
                        LoadInst *Pointer = B.CreateLoad(PointerType::getUnqual(C), Slot);
                        Pointer->setVolatile(true);
                        CallTarget = Pointer;
                    }
                    CallInst *Foreign = B.CreateCall(Target->getFunctionType(), CallTarget, Args);
                    Foreign->setCallingConv(Target->getCallingConv());
                    Foreign->setAttributes(Target->getAttributes());
                    Foreign->setTailCallKind(CallInst::TCK_NoTail);
                    if (NativeDomain) EmitGuard(B, 1U);
                    if (Bridge->getReturnType()->isVoidTy()) B.CreateRetVoid();
                    else B.CreateRet(Foreign);
                }
                Call->setCalledFunction(Bridge);
                F->setMemoryEffects(MemoryEffects::unknown());
                F->removeFnAttr(Attribute::NoSync);
            }
        }
        DenseMap<GlobalVariable *, Function *> TLSBridges;
        for (Function *F : Functions) {
            if ((Effects[F] & Unknown) != 0U || (Effects[F] & Affine) == 0U) continue;
            SmallVector<Instruction *, 32> Original;
            for (Instruction &I : instructions(F)) Original.push_back(&I);
            for (Instruction *I : Original) {
                IRBuilder<> B(I);
                std::function<Value *(Value *)> Resolve = [&](Value *V) -> Value * {
                    if (auto *Global = dyn_cast<GlobalVariable>(V)) {
                        if (!Global->isThreadLocal()) return V;
                        if (Global->getAddressSpace() != 0U) {
                            report_fatal_error("LLAM nondefault TLS address space is unqualified");
                        }
                        Function *&Bridge = TLSBridges[Global];
                        if (Bridge == nullptr) {
                            Bridge = Function::Create(FunctionType::get(Global->getType(), false),
                                GlobalValue::InternalLinkage, "__llam.tls." + Global->getName(), &M);
                            Bridge->addFnAttr(Attribute::NoInline);
                            Bridge->addFnAttr("llam.foreign.effect", "4");
                            Bridge->addFnAttr("llam.managed.class", "0");
                            Bridge->addFnAttr("llam.managed.flags", "9");
                            Bridge->addFnAttr("llam.managed.effects", std::to_string(Affine));
                            Bridge->addFnAttr("llam.managed.type", std::to_string(managedTypeIdentity(*Bridge)));
                            Bridge->addFnAttr("llam.managed.build", BuildId);
                            Bridge->addFnAttr("llam.managed.native", Bridge->getName());
                            Bridge->addFnAttr("llam.managed.bindings", Bindings->getName());
                            IRBuilder<> G(BasicBlock::Create(C, "tls.entry", Bridge));
                            EmitGuard(G, 0U);
                            // Keep TLS lowering inside one explicit native call
                            // on every target. ELF may use a direct thread-pointer
                            // instruction instead of a resolver call; an outlined
                            // accessor gives both forms the same guarded witness.
                            Function *Accessor = Function::Create(Bridge->getFunctionType(),
                                GlobalValue::InternalLinkage, "__llam.tls.address." + Global->getName(), &M);
                            Accessor->addFnAttr(Attribute::NoInline);
                            IRBuilder<> A(BasicBlock::Create(C, "tls.address", Accessor));
                            Function *Address = Intrinsic::getOrInsertDeclaration(&M,
                                Intrinsic::threadlocal_address, {Global->getType()});
                            A.CreateRet(A.CreateCall(Address, {Global}));
                            CallInst *Pointer = G.CreateCall(Accessor);
                            Pointer->setTailCallKind(CallInst::TCK_NoTail);
                            EmitGuard(G, 1U);
                            G.CreateRet(Pointer);
                        }
                        return B.CreateCall(Bridge);
                    }
                    if (auto *Expr = dyn_cast<ConstantExpr>(V)) {
                        if (!referencesTLS(Expr)) return V;
                        Instruction *Expanded = Expr->getAsInstruction();
                        B.Insert(Expanded);
                        for (Use &Operand : Expanded->operands()) Operand.set(Resolve(Operand.get()));
                        return Expanded;
                    }
                    return V;
                };
                if (auto *Intrinsic = dyn_cast<IntrinsicInst>(I)) {
                    if (Intrinsic->getIntrinsicID() == Intrinsic::threadlocal_address) {
                        Value *Pointer = Resolve(Intrinsic->getArgOperand(0));
                        Intrinsic->replaceAllUsesWith(Pointer);
                        Intrinsic->eraseFromParent();
                        continue;
                    }
                }
                for (Use &Operand : I->operands()) {
                    if (auto *Phi = dyn_cast<PHINode>(I)) {
                        B.SetInsertPoint(Phi->getIncomingBlock(Operand)->getTerminator());
                    }
                    if (referencesTLS(Operand.get())) Operand.set(Resolve(Operand.get()));
                }
            }
            F->setMemoryEffects(MemoryEffects::unknown());
            F->removeFnAttr(Attribute::NoSync);
        }
        bool NeedsStackFailure = false;
        for (Function *F : Functions) {
            NeedsStackFailure |= (Effects[F] & Unknown) == 0U &&
                (F->hasFnAttribute(Attribute::StackProtect) ||
                 F->hasFnAttribute(Attribute::StackProtectReq) ||
                 F->hasFnAttribute(Attribute::StackProtectStrong));
        }
        if (NeedsStackFailure) {
            FunctionType *Type = FunctionType::get(Type::getVoidTy(C), false);
            FunctionCallee Fail = M.getOrInsertFunction("__stack_chk_fail", Type);
            Function *Bridge = Function::Create(Type, GlobalValue::InternalLinkage,
                "__llam.bridge.__stack_chk_fail", &M);
            Bridge->addFnAttr(Attribute::NoInline);
            Bridge->addFnAttr(Attribute::NoReturn);
            Bridge->addFnAttr("llam.foreign.effect", "1");
            Bridge->addFnAttr("llam.managed.class", "0");
            Bridge->addFnAttr("llam.managed.flags", "9");
            Bridge->addFnAttr("llam.managed.effects", "2");
            Bridge->addFnAttr("llam.managed.type", std::to_string(managedTypeIdentity(*Bridge)));
            Bridge->addFnAttr("llam.managed.build", BuildId);
            Bridge->addFnAttr("llam.managed.native", Bridge->getName());
            Bridge->addFnAttr("llam.managed.bindings", Bindings->getName());
            appendToCompilerUsed(M, {Bridge});
            IRBuilder<> B(BasicBlock::Create(C, "stack.failure", Bridge));
            EmitGuard(B, 0U);
            B.CreateCall(Fail);
            B.CreateUnreachable();
        }
        return PreservedAnalyses::none();
    }
};
} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "LLAMManagedIR", LLVM_VERSION_STRING,
        [](PassBuilder &PB) {
            PB.registerOptimizerLastEPCallback(
                [](ModulePassManager &PM, OptimizationLevel, ThinOrFullLTOPhase) {
                    PM.addPass(ManagedPass());
                });
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &PM, ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name != "llam-managed") return false;
                    PM.addPass(ManagedPass());
                    return true;
                });
        }};
}
