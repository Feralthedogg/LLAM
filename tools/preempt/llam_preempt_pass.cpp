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

#include "metadata.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Plugins/PassPlugin.h")
#include "llvm/Plugins/PassPlugin.h"
#else
#include "llvm/Passes/PassPlugin.h"
#endif
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>

using namespace llvm;

namespace {

constexpr StringLiteral AsyncAnnotation("llam-preempt-async-safe");
constexpr StringLiteral CooperativeAnnotation("llam-preempt-cooperative");
constexpr StringLiteral AsyncRequestedAttribute("llam.preempt.async_requested");
constexpr StringLiteral CooperativeRequestedAttribute("llam.preempt.cooperative_requested");
constexpr StringLiteral AsyncSafeFunctionAttribute("llam.preempt.async_safe");
constexpr StringLiteral Win64IRAcceptedAttribute("llam.preempt.win64_ir_accepted");
constexpr StringLiteral CooperativeFunctionAttribute("llam.preempt.cooperative");

static bool requiredMode() {
    const char *Value = std::getenv("LLAM_PREEMPT_REQUIRED");

    if (Value == nullptr) {
        Value = std::getenv("LLAM_ASYNC_REQUIRED");
    }
    return Value != nullptr && Value[0] == '1' && Value[1] == '\0';
}

static bool win64AuditMode() {
    const char *Value = std::getenv("LLAM_PREEMPT_WIN64_AUDIT");
    return Value != nullptr && Value[0] == '1' && Value[1] == '\0';
}

struct SafeFunctionRecord {
    Function *Fn;
    std::string SectionName;
};

static uint64_t hashText(StringRef Text) {
    uint64_t Hash = UINT64_C(14695981039346656037);

    for (unsigned char Ch : Text.bytes()) {
        Hash ^= Ch;
        Hash *= UINT64_C(1099511628211);
    }
    return Hash;
}

static std::string formatHex(uint64_t Value) {
    char Buffer[17];
    std::snprintf(Buffer, sizeof(Buffer), "%016llx", (unsigned long long)Value);
    return std::string(Buffer);
}

static bool hasSourceAnnotation(const Function &Fn, StringRef Wanted) {
    const Module *Mod = Fn.getParent();
    const GlobalVariable *Annotations = Mod->getGlobalVariable("llvm.global.annotations");
    const auto *Array = Annotations != nullptr && Annotations->hasInitializer()
                            ? dyn_cast<ConstantArray>(Annotations->getInitializer())
                            : nullptr;

    if (Array == nullptr) {
        return false;
    }
    for (const Use &EntryUse : Array->operands()) {
        const auto *Entry = dyn_cast<ConstantStruct>(EntryUse.get());
        if (Entry == nullptr || Entry->getNumOperands() < 2U ||
            Entry->getOperand(0)->stripPointerCasts() != &Fn) {
            continue;
        }
        const auto *StringGlobal = dyn_cast<GlobalVariable>(
            Entry->getOperand(1)->stripPointerCasts());
        const auto *StringData =
            StringGlobal != nullptr && StringGlobal->hasInitializer()
                ? dyn_cast<ConstantDataArray>(StringGlobal->getInitializer())
                : nullptr;
        if (StringData != nullptr && StringData->isCString() &&
            StringData->getAsCString() == Wanted) {
            return true;
        }
    }
    return false;
}

static bool verifyAsyncSafeFunction(const Function &Fn, std::string &WhyNot) {
    if (Fn.isDeclaration() || Fn.hasFnAttribute(Attribute::Naked) ||
        Fn.hasFnAttribute(Attribute::ReturnsTwice) ||
        Fn.hasFnAttribute(Attribute::Convergent)) {
        WhyNot = "unsupported function attribute or declaration";
        return false;
    }
    for (const Instruction &Inst : instructions(Fn)) {
        if (isa<AllocaInst>(Inst) || isa<InvokeInst>(Inst) ||
            isa<CallBrInst>(Inst) || isa<AtomicRMWInst>(Inst) ||
            isa<AtomicCmpXchgInst>(Inst) || isa<FenceInst>(Inst) ||
            isa<IndirectBrInst>(Inst) || isa<ResumeInst>(Inst) ||
            isa<VAArgInst>(Inst) ||
            isa<LandingPadInst>(Inst) || isa<CatchSwitchInst>(Inst) ||
            isa<CatchPadInst>(Inst) || isa<CleanupPadInst>(Inst)) {
            WhyNot = "contains stack allocation, indirect control flow, exception handling, or an atomic mutation";
            return false;
        }
        if (const auto *Call = dyn_cast<CallBase>(&Inst)) {
            const Function *Callee = Call->getCalledFunction();
            if (Callee != nullptr && Callee->isIntrinsic() &&
                (Callee->getIntrinsicID() == Intrinsic::dbg_value ||
                 Callee->getIntrinsicID() == Intrinsic::dbg_declare ||
                 Callee->getIntrinsicID() == Intrinsic::dbg_label ||
                 Callee->getIntrinsicID() == Intrinsic::lifetime_start ||
                 Callee->getIntrinsicID() == Intrinsic::lifetime_end)) {
                continue;
            }
            WhyNot = "contains a call or inline assembly";
            return false;
        }
        if (const auto *Load = dyn_cast<LoadInst>(&Inst)) {
            (void)Load;
            WhyNot = "contains a memory read";
            return false;
        }
        if (const auto *Store = dyn_cast<StoreInst>(&Inst)) {
            (void)Store;
            WhyNot = "contains a memory write";
            return false;
        }
    }
    return true;
}

static void collectLoopLatches(Loop &L, SmallPtrSetImpl<BasicBlock *> &Latches) {
    BasicBlock *Latch = L.getLoopLatch();

    if (Latch != nullptr) {
        Instruction *Terminator = Latch->getTerminator();
#if LLVM_VERSION_MAJOR >= 23
        bool IsBranch = isa<UncondBrInst>(Terminator) ||
                        isa<CondBrInst>(Terminator);
#else
        bool IsBranch = isa<BranchInst>(Terminator);
#endif
        if (IsBranch &&
            !Latch->isEHPad() && !Terminator->isEHPad()) {
            Latches.insert(Latch);
        }
    }
    for (Loop *Subloop : L.getSubLoops()) {
        collectLoopLatches(*Subloop, Latches);
    }
}

class LLAMPreemptAnnotationSeedPass
    : public PassInfoMixin<LLAMPreemptAnnotationSeedPass> {
public:
    PreservedAnalyses run(Module &Mod, ModuleAnalysisManager &) {
        bool Changed = false;

        for (Function &Fn : Mod) {
            if (Fn.isDeclaration()) {
                continue;
            }
            if (hasSourceAnnotation(Fn, AsyncAnnotation)) {
                Fn.addFnAttr(AsyncRequestedAttribute);
                Fn.addFnAttr(Attribute::NoInline);
                Changed = true;
            }
            if (hasSourceAnnotation(Fn, CooperativeAnnotation)) {
                Fn.addFnAttr(CooperativeRequestedAttribute);
                Changed = true;
            }
        }
        return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};

class LLAMPreemptFunctionPass : public PassInfoMixin<LLAMPreemptFunctionPass> {
public:
    PreservedAnalyses run(Function &Fn, FunctionAnalysisManager &AM) {
        if (Fn.isDeclaration()) {
            return PreservedAnalyses::all();
        }

        bool WantsAsync = Fn.hasFnAttribute(AsyncRequestedAttribute) ||
                          hasSourceAnnotation(Fn, AsyncAnnotation);
        bool WantsCooperative = Fn.hasFnAttribute(CooperativeRequestedAttribute) ||
                                hasSourceAnnotation(Fn, CooperativeAnnotation);
        if (Fn.hasFnAttribute(AsyncSafeFunctionAttribute) ||
            Fn.hasFnAttribute(Win64IRAcceptedAttribute)) {
            WantsAsync = false;
        }
        if (Fn.hasFnAttribute(CooperativeFunctionAttribute)) {
            WantsCooperative = false;
        }
        if (!WantsAsync && !WantsCooperative) {
            return PreservedAnalyses::all();
        }
        if (WantsAsync && WantsCooperative) {
            if (requiredMode()) {
                report_fatal_error("LLAM ASYNC_REQUIRED function cannot also request cooperative poll insertion");
            }
            errs() << "llam-preempt: ignoring conflicting annotations on " << Fn.getName() << "\n";
            return PreservedAnalyses::all();
        }

        Module &Mod = *Fn.getParent();
        LLVMContext &Context = Mod.getContext();
        bool Changed = false;

        if (WantsCooperative) {
            SmallPtrSet<BasicBlock *, 8> Latches;
            LoopInfo &Loops = AM.getResult<LoopAnalysis>(Fn);

            for (Loop *TopLevel : Loops) {
                collectLoopLatches(*TopLevel, Latches);
            }
            if (Latches.empty()) {
                if (requiredMode()) {
                    report_fatal_error("LLAM ASYNC_REQUIRED cooperative annotation has no legal loop latch");
                }
                errs() << "llam-preempt: no natural-loop latch in " << Fn.getName() << "\n";
                return PreservedAnalyses::all();
            }
            FunctionCallee Safepoint = Mod.getOrInsertFunction(
                "llam_task_safepoint",
                FunctionType::get(Type::getVoidTy(Context), false));
            for (BasicBlock *Latch : Latches) {
                IRBuilder<> Builder(Latch->getTerminator());
                Builder.CreateCall(Safepoint);
            }
            Fn.addFnAttr(CooperativeFunctionAttribute);
            Mod.getOrInsertNamedMetadata("llam.preempt.cooperative_polls")
                ->addOperand(MDNode::get(Context, {}));
            Changed = true;
        }

        if (WantsAsync) {
            Triple Target(Mod.getTargetTriple());
            bool SupportedLinux = Target.isOSLinux() &&
                                  Target.getArch() == Triple::x86_64;
            bool SupportedWindowsAudit = win64AuditMode() &&
                                         Target.isWindowsGNUEnvironment() &&
                                         Target.isOSBinFormatCOFF() &&
                                         Target.getArch() == Triple::x86_64;
            bool SupportedTarget = SupportedLinux || SupportedWindowsAudit;
            std::string WhyNot;

            if (!SupportedTarget || !verifyAsyncSafeFunction(Fn, WhyNot)) {
                if (requiredMode()) {
                    report_fatal_error(Twine("LLAM ASYNC_REQUIRED rejected ") + Fn.getName() +
                                       (SupportedTarget ? Twine(": ") + WhyNot
                                                        : Twine(": unsupported target")));
                }
                errs() << "llam-preempt: ignoring async-safe annotation on " << Fn.getName()
                       << (SupportedTarget ? Twine(": ") + WhyNot
                                           : Twine(": unsupported target"))
                       << "\n";
                return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
            }

            Fn.addFnAttr(Attribute::NoInline);
            if (SupportedWindowsAudit) {
                /* Diagnostic-only provenance: the final Win64 machine body
                 * still needs independent codegen/layout qualification. */
                Fn.addFnAttr(Win64IRAcceptedAttribute);
                Mod.getOrInsertNamedMetadata("llam.preempt.win64_ir_accepted")
                    ->addOperand(MDNode::get(Context, ValueAsMetadata::get(&Fn)));
            } else {
                uint64_t Hash = hashText(Mod.getModuleIdentifier());
                Hash ^= hashText(Fn.getName());
                Hash *= UINT64_C(1099511628211);
                std::string SectionName = "llam_preempt_safe_" + formatHex(Hash);

                Fn.addFnAttr(AsyncSafeFunctionAttribute);
                Fn.setSection(SectionName);
                Mod.getOrInsertNamedMetadata("llam.preempt.async_safe")
                    ->addOperand(MDNode::get(Context, ValueAsMetadata::get(&Fn)));
            }
            Changed = true;
        }
        return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};

class LLAMPreemptMetadataPass : public PassInfoMixin<LLAMPreemptMetadataPass> {
public:
    PreservedAnalyses run(Module &Mod, ModuleAnalysisManager &) {
        NamedMDNode *SafeFunctions = Mod.getNamedMetadata("llam.preempt.async_safe");
        NamedMDNode *CooperativePolls = Mod.getNamedMetadata("llam.preempt.cooperative_polls");
        SmallVector<SafeFunctionRecord, 8> Ranges;
        LLVMContext &Context = Mod.getContext();
        Triple Target(Mod.getTargetTriple());
        bool HasCooperativePolls = CooperativePolls != nullptr &&
                                   CooperativePolls->getNumOperands() != 0U;
        uint16_t Architecture = LLAM_PREEMPT_METADATA_ARCH_UNKNOWN;

        if (Target.isOSWindows()) {
            /* Windows diagnostic mode records no LLPM certificate. The
             * pinned X86 codegen gate owns any eventual COFF emission. */
            return PreservedAnalyses::all();
        }

        if (Target.getArch() == Triple::x86_64) {
            Architecture = LLAM_PREEMPT_METADATA_ARCH_X86_64;
        } else if (Target.getArch() == Triple::aarch64) {
            Architecture = LLAM_PREEMPT_METADATA_ARCH_AARCH64;
        }
        if (SafeFunctions != nullptr) {
            for (MDNode *Entry : SafeFunctions->operands()) {
                if (Entry->getNumOperands() == 0U) {
                    continue;
                }
                auto *Value = dyn_cast_or_null<ValueAsMetadata>(Entry->getOperand(0));
                auto *Fn = Value != nullptr ? dyn_cast<Function>(Value->getValue()) : nullptr;
                if (Fn == nullptr || Fn->getSection().empty()) {
                    continue;
                }
                Ranges.push_back({Fn, Fn->getSection().str()});
            }
        }
        if (Ranges.empty() && !HasCooperativePolls) {
            return PreservedAnalyses::all();
        }

        StructType *HeaderType = StructType::get(
            Context,
            {Type::getInt32Ty(Context),
             Type::getInt16Ty(Context),
             Type::getInt16Ty(Context),
             Type::getInt32Ty(Context),
             Type::getInt32Ty(Context),
             Type::getInt64Ty(Context),
             Type::getInt32Ty(Context),
             Type::getInt32Ty(Context)});
        StructType *RangeType = StructType::get(
            Context,
            {Type::getInt32Ty(Context),
             Type::getInt32Ty(Context),
             Type::getInt32Ty(Context),
             Type::getInt32Ty(Context)});
        ArrayType *RangeArrayType = ArrayType::get(RangeType, Ranges.size());
        StructType *RecordType = StructType::get(Context, {HeaderType, RangeArrayType});
        std::string Name = "__llam_preempt_map_" + formatHex(hashText(Mod.getModuleIdentifier()));
        if (Mod.getGlobalVariable(Name, true) != nullptr) {
            return PreservedAnalyses::all();
        }
        auto *Map = new GlobalVariable(Mod,
                                       RecordType,
                                       true,
                                       GlobalValue::InternalLinkage,
                                       nullptr,
                                       Name);
        Map->setSection(LLAM_PREEMPT_METADATA_SECTION);
        Map->setAlignment(Align(8));

        Type *I8 = Type::getInt8Ty(Context);
        Type *I32 = Type::getInt32Ty(Context);
        Type *I64 = Type::getInt64Ty(Context);
        Constant *Zero32 = ConstantInt::get(I32, 0U);
        Constant *One32 = ConstantInt::get(I32, 1U);
        Constant *Header = ConstantStruct::get(
            HeaderType,
            {ConstantInt::get(I32, LLAM_PREEMPT_METADATA_MAGIC),
             ConstantInt::get(Type::getInt16Ty(Context), LLAM_PREEMPT_METADATA_VERSION),
             ConstantInt::get(Type::getInt16Ty(Context), Architecture),
             ConstantInt::get(I32, LLAM_PREEMPT_METADATA_HEADER_SIZE),
             ConstantInt::get(I32,
                              LLAM_PREEMPT_METADATA_HEADER_SIZE +
                                  Ranges.size() * LLAM_PREEMPT_METADATA_RANGE_SIZE),
             ConstantInt::get(I64, hashText(Mod.getModuleIdentifier())),
             ConstantInt::get(I32, Ranges.size()),
             ConstantInt::get(I32,
                              (requiredMode()
                                   ? (uint32_t)LLAM_PREEMPT_METADATA_F_REQUIRED
                                   : 0U) |
                                  (HasCooperativePolls
                                       ? (uint32_t)LLAM_PREEMPT_METADATA_F_COOPERATIVE_POLLS
                                       : 0U) |
                                  (!Ranges.empty()
                                       ? (uint32_t)LLAM_PREEMPT_METADATA_F_ASYNC_RANGES
                                       : 0U))});
        SmallVector<Constant *, 8> RangeConstants;
        for (size_t Index = 0; Index < Ranges.size(); ++Index) {
            const SafeFunctionRecord &Record = Ranges[Index];
            std::string StartName = "__start_" + Record.SectionName;
            std::string EndName = "__stop_" + Record.SectionName;
            GlobalVariable *Start = Mod.getGlobalVariable(StartName, true);
            GlobalVariable *End = Mod.getGlobalVariable(EndName, true);

            if (Start == nullptr) {
                Start = new GlobalVariable(Mod, I8, false, GlobalValue::ExternalLinkage,
                                           nullptr, StartName);
            }
            if (End == nullptr) {
                End = new GlobalVariable(Mod, I8, false, GlobalValue::ExternalLinkage,
                                         nullptr, EndName);
            }
            Constant *StartIndices[] = {Zero32, One32,
                                        ConstantInt::get(I32, Index), Zero32};
            Constant *EndIndices[] = {Zero32, One32,
                                      ConstantInt::get(I32, Index), One32};
            Constant *StartField = ConstantExpr::getGetElementPtr(
                RecordType, Map, ArrayRef<Constant *>(StartIndices));
            Constant *EndField = ConstantExpr::getGetElementPtr(
                RecordType, Map, ArrayRef<Constant *>(EndIndices));
            Constant *StartDelta = ConstantExpr::getSub(
                ConstantExpr::getPtrToInt(Start, I64),
                ConstantExpr::getPtrToInt(StartField, I64));
            Constant *EndDelta = ConstantExpr::getSub(
                ConstantExpr::getPtrToInt(End, I64),
                ConstantExpr::getPtrToInt(EndField, I64));
            Constant *StartRelative = ConstantExpr::getTrunc(StartDelta, I32);
            Constant *EndRelative = ConstantExpr::getTrunc(EndDelta, I32);

            RangeConstants.push_back(ConstantStruct::get(
                RangeType,
                {StartRelative,
                 EndRelative,
                 ConstantInt::get(I32, LLAM_PREEMPT_RANGE_ASYNC_SAFE),
                 Zero32}));
        }
        Constant *RangeArray = ConstantArray::get(RangeArrayType, RangeConstants);
        Map->setInitializer(ConstantStruct::get(RecordType, {Header, RangeArray}));
        appendToUsed(Mod, {Map});
        return PreservedAnalyses::none();
    }
};

static void addLLAMPreemptPasses(ModulePassManager &MPM) {
    MPM.addPass(LLAMPreemptAnnotationSeedPass());
    MPM.addPass(createModuleToFunctionPassAdaptor(LLAMPreemptFunctionPass()));
    MPM.addPass(LLAMPreemptMetadataPass());
}

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION,
            "LLAMPreempt",
            LLVM_VERSION_STRING,
            [](PassBuilder &PB) {
                PB.registerPipelineStartEPCallback(
                    [](ModulePassManager &MPM, OptimizationLevel) {
                        MPM.addPass(LLAMPreemptAnnotationSeedPass());
                    });
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, ModulePassManager &MPM,
                       ArrayRef<PassBuilder::PipelineElement>) {
                        if (Name != "llam-preempt") {
                            return false;
                        }
                        addLLAMPreemptPasses(MPM);
                        return true;
                    });
                PB.registerOptimizerLastEPCallback(
                    [](ModulePassManager &MPM, OptimizationLevel, auto...) {
                        addLLAMPreemptPasses(MPM);
                    });
            }};
}
