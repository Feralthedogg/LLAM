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
 * See LICENSES/Apache-2.0.txt.
 */

#include "metadata_v2.h"
#include "managed_llvm_contract.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Pass.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Regex.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdlib>
#include <iterator>
#include <string>

using namespace llvm;

namespace {
struct Interval {
    MCSymbol *Begin;
    MCSymbol *End;
    unsigned Kind;
};

struct CallRecord {
    MCSymbol *Begin;
    MCSymbol *End;
    std::string Target;
    uint64_t Type;
    unsigned Effect;
    unsigned Flags = 0U;
    uint64_t Contract = 0U;
};

// X86AsmPrinter inserts a Windows unwind NOP after a call immediately before an
// epilogue (or the end of a function). Its MI post-symbol then follows the NOP,
// rather than the hardware return PC. Materialize that same padding before
// placing witnesses, so AsmPrinter sees a real continuation instruction and
// leaves the exact CALL end label intact.
void materializeWindowsCallPadding(MachineFunction &MF, const TargetInstrInfo &TII) {
    if (!MF.getFunction().getParent()->getTargetTriple().isOSWindows()) return;
    for (MachineBasicBlock &BB : MF) {
        for (auto It = BB.begin(); It != BB.end(); ++It) {
            if (!It->isCall()) continue;
            MachineBasicBlock *NextBlock = &BB;
            auto Next = std::next(It);
            bool NeedsPadding = false;
            for (;;) {
                if (Next == NextBlock->end()) {
                    NextBlock = NextBlock->getNextNode();
                    if (NextBlock == nullptr) { NeedsPadding = true; break; }
                    Next = NextBlock->begin();
                    continue;
                }
                if (TII.getName(Next->getOpcode()) == "SEH_BeginEpilogue" ||
                    (Next->isEHLabel() && MF.getFunction().hasPersonalityFn())) {
                    NeedsPadding = true;
                    break;
                }
                if (!Next->isPseudo() && !Next->isMetaInstruction()) break;
                ++Next;
            }
            if (NeedsPadding) TII.insertNoop(BB, std::next(It));
        }
    }
}

int guardLoad(const MachineInstr &MI, const GlobalVariable *Bindings, const DataLayout &DL) {
    for (const MachineMemOperand *Memory : MI.memoperands()) {
        if (!Memory->isLoad() || !Memory->isVolatile() || Memory->getValue() == nullptr ||
            !Memory->getSize().hasValue() || Memory->getSize().getValue().getKnownMinValue() != 8U) continue;
        int64_t Offset = 0;
        const Value *Base = GetPointerBaseWithConstantOffset(Memory->getValue(), Offset, DL);
        if (Base != Bindings) continue;
        Offset += Memory->getOffset();
        if (Offset == 0 || Offset == 8) return (int)(Offset / 8);
    }
    return -1;
}

int guardCall(const MachineInstr &Call, const GlobalVariable *Bindings,
              const TargetRegisterInfo &TRI, const TargetInstrInfo &TII, const DataLayout &DL) {
    int Slot = guardLoad(Call, Bindings, DL); // Folded x86 CALL64m.
    if (Slot >= 0) return Slot;
    Register Target;
    for (const MachineOperand &O : Call.operands()) {
        if (O.isReg() && !O.isDef() && !O.isImplicit()) { Target = O.getReg(); break; }
    }
    if (!Target) return -1;
    auto It = Call.getIterator();
    const MachineBasicBlock &BB = *Call.getParent();
    unsigned Steps = 0U;
    while (It != BB.begin() && Steps++ < 64U) {
        const MachineInstr &MI = *--It;
        if (!MI.modifiesRegister(Target, &TRI)) continue;
        Slot = guardLoad(MI, Bindings, DL);
        if (Slot >= 0) return Slot;
        StringRef Name = TII.getName(MI.getOpcode());
        if (Name != "COPY" && Name != "MOV64rr" && Name != "ORRXrs") return -1;
        Register Source;
        for (const MachineOperand &O : MI.operands()) {
            if (O.isReg() && !O.isDef() && !O.isImplicit() && O.getReg() &&
                StringRef(TRI.getName(O.getReg())) != "XZR") {
                if (Source && Source != O.getReg()) return -1;
                Source = O.getReg();
            } else if (O.isImm() && O.getImm() != 0) return -1;
        }
        if (!Source) return -1;
        Target = Source;
    }
    return -1;
}

uint64_t number(const Function &F, StringRef Name) {
    uint64_t Value = 0U;
    if (!F.hasFnAttribute(Name) || F.getFnAttribute(Name).getValueAsString().getAsInteger(10, Value)) {
        report_fatal_error(Twine("LLAM missing/invalid compiler contract: ") + Name);
    }
    return Value;
}

std::string symbolName(const Twine &Name, const DataLayout &DL) {
    std::string Result;
    raw_string_ostream OS(Result);
    Mangler::getNameWithPrefix(OS, Name, DL);
    return Result;
}

// This is deliberately a positive instruction profile. An unfamiliar target
// opcode never becomes SAFE because it happens to lack LLVM's call flag.
bool ordinaryOpcode(StringRef Name, bool Arm) {
    static const Regex ArmProfile(
        "^((ADD|ADDS|SUB|SUBS)[WX](ri|rs|rx)|"
        "(AND|ANDS|ORR|EOR|BIC|ORN|EON)[WX](ri|rs)|"
        "(LSLV|LSRV|ASRV|RORV)[WX]r|(UBFM|SBFM|BFM)[WX]ri|EXTR[WX]rri|"
        "(MADD|MSUB)[WX]rrr|(SMADDL|UMADDL|SMSUBL|UMSUBL)rrr|"
        "(SMULH|UMULH)rr|(UDIV|SDIV)[WX]r|(MOVZ|MOVN|MOVK)[WX]i|"
        "(CSEL|CSINC|CSINV|CSNEG)[WX]r|(CCMP|CCMN)[WX][ri]|(CLZ|CLS|RBIT|REV|REV16|REV32)[WX]r|"
        "(LDR|STR)(B|H|W|X|S|D|Q)(ui|i|roW|roX|pre|post)|"
        "(LDRB|LDRH|STRB|STRH)(B|H)(ui|roW|roX|pre|post)|"
        "(LDUR|STUR)(B|H|W|X|S|D|Q)i|LDR(SW|SBW|SBX|SHW|SHX)(ui|roW|roX)|"
        "(LDP|STP)(W|X|S|D|Q)(i|pre|post)|"
        "(LDAR|STLR)(B|H|W|X)|(LDADD|LDCLR|LDEOR|LDSET|SWP|CAS)(A|L|AL)?[BHWX]|"
        "ADR|ADRP|B|Bcc|CB(NZ|Z)[WX]|TB(NZ|Z)[WX]|HINT|"
        "(FADD|FSUB|FMUL|FDIV|FMAX|FMIN|FMAXNM|FMINNM|FNMUL)[SD]rr|"
        "(FABS|FNEG|FSQRT)[SD]r|FMOV[SD](r|i)|FMOV[WX][SD]r|FMOV[SD][WX]r|"
        "(FCMP|FCMPE)[SD](rr|ri)|FCSEL[SD]rrr|FCVT[SD][SD]r|"
        "(SCVTF|UCVTF)[SD][WX]ri|(FCVTZS|FCVTZU)[WX][SD]r|"
        "(FMADD|FMSUB|FNMADD|FNMSUB)[SD]rrr|"
        "(ADD|SUB|MUL|AND|ORR|EOR|BIC|CMEQ|CMGT|CMHI|SMAX|SMIN|UMAX|UMIN)"
        "v(8i8|16i8|4i16|8i16|2i32|4i32|2i64)|"
        "(FADD|FSUB|FMUL|FDIV|FMAX|FMIN)v(2f32|4f32|2f64)|"
        "(DUP|INS|UMOV|SMOV|EXT|MOVI|MVNI|ADDV|ADDP|UADDLV|SADDLV)[A-Za-z0-9_]*|"
        "COPY|KILL|IMPLICIT_DEF)$");
    static const Regex X86Profile(
        "^((MOV|MOVSX|MOVZX|MOVSXD|LEA|ADD|ADC|SUB|SBB|AND|OR|XOR|CMP|TEST|"
        "INC|DEC|NEG|NOT|SHL|SHR|SAR|ROL|ROR|IMUL|MUL|IDIV|DIV|CMOV|SET|"
        "XCHG|XADD|CMPXCHG|LOCK_)[A-Za-z0-9_]*|L(XADD|CMPXCHG)(8|16|32|64)|"
        "(PUSH|POP)(16|32|64)[A-Za-z0-9_]*|JCC_[124]|JMP_[124]|"
        "(CDQ|CQO|CWD|CBW|CWDE|CDQE)|NOOP[A-Za-z0-9_]*|"
        "(MOVAPS|MOVUPS|MOVAPD|MOVUPD|MOVSS|MOVSD|MOVDQA|MOVDQU|MOVD|MOVQ|"
        "ADDSS|ADDSD|ADDPS|ADDPD|SUBSS|SUBSD|SUBPS|SUBPD|MULSS|MULSD|MULPS|MULPD|"
        "DIVSS|DIVSD|DIVPS|DIVPD|SQRTSS|SQRTSD|SQRTPS|SQRTPD|UCOMISS|UCOMISD|"
        "CVTSS2SD|CVTSD2SS|CVTSI2SS|CVTSI2SD|CVTTSS2SI|CVTTSD2SI|"
        "PXOR|PAND|POR|PADD|PSUB|PMUL|PSLL|PSRL|PSRA|PSHUFD|PSHUFB|"
        "XORPS|XORPD|ANDPS|ANDPD|ORPS|ORPD|SHUFPS|SHUFPD|UNPCK|PACK|PUNPCK|"
        "PCMPEQ|PCMPGT|PMIN|PMAX)[A-Za-z0-9_]*|COPY|KILL|IMPLICIT_DEF)$");
    return (Arm ? ArmProfile : X86Profile).match(Name);
}

bool forbiddenRegister(const MachineInstr &MI, const TargetRegisterInfo &TRI, bool Arm) {
    for (const MachineOperand &Operand : MI.operands()) {
        if (!Operand.isReg() || !Operand.getReg()) continue;
        Register Reg = Operand.getReg();
        if (!Reg.isPhysical()) return true;
        StringRef Name = TRI.getName(Reg);
        // LLVM models an implicit SSP use/def on every x86 CALL, including
        // ordinary targets with CET disabled. It is not an explicit shadow-stack
        // operation; runtime capability admission separately rejects active CET.
        if (!Arm && Name == "SSP" && Operand.isImplicit() && (MI.isCall() || MI.isReturn())) continue;
        if (Arm) {
            if (Name.starts_with("Z") || Name.starts_with("P") || Name == "FFR" ||
                Name == "ZA" || Name == "X18" || Name == "W18") return true;
        } else if (Name.starts_with("YMM") || Name.starts_with("ZMM") ||
                   Name.starts_with("K") || Name.starts_with("TMM") ||
                   Name.starts_with("ST") || Name == "FS" || Name == "GS" ||
                   Name == "SSP" || Name == "PKRU") return true;
    }
    return false;
}

unsigned callEffect(const Function *F) {
    if (F == nullptr) return LLAM_LLPM2_EFFECT_UNKNOWN;
    if (F->hasFnAttribute("llam.foreign.effect")) return (unsigned)number(*F, "llam.foreign.effect");
    if (F->hasFnAttribute("llam.managed.class")) {
        return number(*F, "llam.managed.class") == LLAM_LLPM2_LEGACY
            ? LLAM_LLPM2_EFFECT_UNKNOWN : LLAM_LLPM2_EFFECT_MANAGED;
    }
    // External calls must already have a typed IR bridge. A late libc/helper
    // call has no effect proof, even if its spelling is familiar.
    return LLAM_LLPM2_EFFECT_UNKNOWN;
}

uint64_t typeIdentity(const Function &F) {
    if (F.hasFnAttribute("llam.managed.type")) return number(F, "llam.managed.type");
    return llam::managedTypeIdentity(F);
}

class ManagedMachinePass final : public MachineFunctionPass {
    bool AnchorEmitted = false;
public:
    static char ID;
    ManagedMachinePass() : MachineFunctionPass(ID) {}

    bool runOnMachineFunction(MachineFunction &MF) override {
        Function &F = MF.getFunction();
        if (!F.hasFnAttribute("llam.managed.class")) return false;
        Triple T(F.getParent()->getTargetTriple());
        bool Arm = T.getArch() == Triple::aarch64 || T.getArch() == Triple::aarch64_32;
        if ((!Arm && T.getArch() != Triple::x86_64) || T.getArch() == Triple::aarch64_32) {
            report_fatal_error("LLAM managed machine profile requires 64-bit AArch64 or x86-64");
        }
        const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
        const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();
        unsigned Class = (unsigned)number(F, "llam.managed.class");
        unsigned Effects = (unsigned)number(F, "llam.managed.effects");
        unsigned EntryFlags = F.hasFnAttribute("llam.managed.flags")
            ? (unsigned)number(F, "llam.managed.flags") : 0U;
        if (F.hasFnAttribute(Attribute::NoReturn)) EntryFlags |= LLAM_LLPM2_ENTRY_NORETURN;
        GlobalVariable *Bindings = F.hasFnAttribute("llam.managed.bindings")
            ? F.getParent()->getNamedGlobal(F.getFnAttribute("llam.managed.bindings").getValueAsString()) : nullptr;
        GlobalVariable *RuntimeBinding = F.hasFnAttribute("llam.runtime.binding")
            ? F.getParent()->getNamedGlobal(F.getFnAttribute("llam.runtime.binding").getValueAsString()) : nullptr;
        if (Class != LLAM_LLPM2_LEGACY ||
            (EntryFlags & (LLAM_LLPM2_ENTRY_NATIVE_BOUNDARY | LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE)) != 0U) {
            materializeWindowsCallPadding(MF, TII);
        }
        uint64_t ISA = LLAM_LLPM2_ISA_BASE | LLAM_LLPM2_ISA_FP_SIMD;
        std::string Failure;
        if (Class != LLAM_LLPM2_LEGACY) {
            for (MachineBasicBlock &BB : MF) {
                for (MachineInstr &MI : BB) {
                    if (!MI.isCall()) continue;
                    for (MachineOperand &O : MI.operands()) {
                        if (O.isSymbol() && StringRef(O.getSymbolName()) == "__stack_chk_fail") {
                            Function *Bridge = F.getParent()->getFunction("__llam.bridge.__stack_chk_fail");
                            if (Bridge != nullptr) O.ChangeToGA(Bridge, 0, O.getTargetFlags());
                        }
                    }
                }
            }
        }
        if (F.hasSection() || (MF.getJumpTableInfo() != nullptr &&
            !MF.getJumpTableInfo()->getJumpTables().empty())) Failure = "custom section/jump table";
        for (const MachineBasicBlock &BB : MF) {
            for (const MachineInstr &MI : BB) {
                if (MI.isMetaInstruction()) continue;
                StringRef Name = TII.getName(MI.getOpcode());
                // Exclusive reservations are not part of the saved register
                // image. Do not certify an LL/SC continuation or clear it by guess.
                if (Arm && (Name.contains("LDXR") || Name.contains("LDAXR") ||
                            Name.contains("STXR") || Name.contains("STLXR") ||
                            Name.contains("LDXP") || Name.contains("STXP"))) {
                    Failure = "live exclusive reservation";
                }
                if (MI.isInlineAsm() || forbiddenRegister(MI, TRI, Arm)) Failure = Name.str();
                if (MI.isCall()) {
                    const Function *Target = nullptr;
                    for (const MachineOperand &O : MI.operands()) {
                        if (O.isGlobal()) Target = dyn_cast<Function>(O.getGlobal());
                    }
                    if (callEffect(Target) == LLAM_LLPM2_EFFECT_UNKNOWN) Failure = "unknown native call";
                } else if (!MI.isReturn() && !ordinaryOpcode(Name, Arm)) Failure = Name.str();
                if (Arm && (Name.starts_with("LDADD") || Name.starts_with("LDCLR") ||
                    Name.starts_with("LDEOR") || Name.starts_with("LDSET") ||
                    Name.starts_with("SWP") || Name.starts_with("CAS"))) ISA |= LLAM_LLPM2_ISA_LSE;
            }
        }
        if (Class != LLAM_LLPM2_LEGACY && !Failure.empty()) {
            if (std::getenv("LLAM_MANAGED_REQUIRED") != nullptr) {
                report_fatal_error(Twine("LLAM final-machine state is unqualified in ") +
                                   F.getName() + ": " + Failure);
            }
            Class = LLAM_LLPM2_LEGACY;
            Effects |= 1U << LLAM_LLPM2_EFFECT_UNKNOWN;
        }

        MCContext &Context = MF.getContext();
        SmallVector<Interval, 32> Ranges;
        SmallVector<CallRecord, 8> Calls;
        unsigned BoundaryPhase = 0U;
        unsigned RuntimeCalls = 0U;
        MCSymbol *End = nullptr;
        MachineInstr *LastBodyInstruction = nullptr;
        for (MachineBasicBlock &BB : MF) {
            for (auto It = BB.begin(); It != BB.end();) {
                MachineInstr &MI = *It++;
                if (MI.isMetaInstruction()) continue;
                LastBodyInstruction = &MI;
                if (MI.getPreInstrSymbol() != nullptr || MI.getPostInstrSymbol() != nullptr) {
                    report_fatal_error("LLAM cannot replace existing instruction witnesses");
                }
                MCSymbol *Begin = nullptr;
                MCSymbol *After = nullptr;
                if (Class != LLAM_LLPM2_LEGACY ||
                    (MI.isCall() && (EntryFlags & (LLAM_LLPM2_ENTRY_NATIVE_BOUNDARY |
                                                LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE)) != 0U)) {
                    Begin = Context.createNamedTempSymbol("llam_pc");
                    After = Context.createNamedTempSymbol("llam_pc_end");
                    MI.setPreInstrSymbol(MF, Begin);
                    MI.setPostInstrSymbol(MF, After);
                }
                unsigned Kind = Class == LLAM_LLPM2_LEGACY ? LLAM_LLPM2_UNSAFE_NO_PREEMPT : LLAM_LLPM2_SAFE;
                if (MI.getFlag(MachineInstr::FrameSetup) || MI.getFlag(MachineInstr::FrameDestroy)) {
                    Kind = LLAM_LLPM2_UNSAFE_STACK;
                } else if (MI.isReturn()) Kind = LLAM_LLPM2_UNSAFE_NATIVE_ENTRY;
                else if (MI.isCall()) Kind = LLAM_LLPM2_UNSAFE_FOREIGN;
                if (Class != LLAM_LLPM2_LEGACY) Ranges.push_back({Begin, After, Kind});
                if (MI.isCall() && Class != LLAM_LLPM2_LEGACY) {
                    const Function *Target = nullptr;
                    for (const MachineOperand &O : MI.operands()) {
                        if (O.isGlobal()) Target = dyn_cast<Function>(O.getGlobal());
                    }
                    unsigned Effect = callEffect(Target);
                    Effects |= 1U << Effect;
                    StringRef TargetName = Target->hasFnAttribute("llam.managed.identity")
                        ? Target->getFnAttribute("llam.managed.identity").getValueAsString()
                        : (Target->hasFnAttribute("llam.managed.native")
                           ? Target->getFnAttribute("llam.managed.native").getValueAsString() : Target->getName());
                    Calls.push_back({Begin, After, symbolName(TargetName, MF.getDataLayout()),
                                     typeIdentity(*Target), Effect});
                } else if (MI.isCall() && (EntryFlags & LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE) != 0U) {
                    if (RuntimeBinding == nullptr || RuntimeCalls++ != 0U ||
                        guardCall(MI, RuntimeBinding, TRI, TII, MF.getDataLayout()) != 0) {
                        report_fatal_error("LLAM runtime call does not use its immutable SDK binding");
                    }
                    Calls.push_back({Begin, After, symbolName(RuntimeBinding->getName(), MF.getDataLayout()),
                        number(F, "llam.managed.type"), (unsigned)number(F, "llam.foreign.effect"),
                        LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_RUNTIME_BODY,
                        number(F, "llam.runtime.contract")});
                } else if (MI.isCall() && (EntryFlags & LLAM_LLPM2_ENTRY_NATIVE_BOUNDARY) != 0U) {
                    if (Bindings == nullptr) report_fatal_error("LLAM foreign boundary has no native guard bindings");
                    int Guard = guardCall(MI, Bindings, TRI, TII, MF.getDataLayout());
                    std::string BindingSymbol = symbolName(Bindings->getName(), MF.getDataLayout());
                    if (Guard == 0 && BoundaryPhase == 0U) {
                        Calls.push_back({Begin, After, BindingSymbol,
                            llam::managedTypeIdentity(*F.getParent()->getFunction("llam_managed_native_enter")),
                            LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL, LLAM_LLPM2_CALL_NATIVE_THUNK |
                            LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_GUARD_ENTER});
                        BoundaryPhase = 1U;
                    } else if (Guard < 0 && BoundaryPhase == 1U) {
                        Calls.push_back({Begin, After, "", number(F, "llam.managed.type"),
                            (unsigned)number(F, "llam.foreign.effect"), LLAM_LLPM2_CALL_NATIVE_THUNK |
                            LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_FOREIGN_BODY});
                        BoundaryPhase = 2U;
                    } else if (Guard == 1 && BoundaryPhase == 2U &&
                               (EntryFlags & LLAM_LLPM2_ENTRY_NORETURN) == 0U) {
                        Calls.push_back({Begin, After, BindingSymbol + " + 8",
                            llam::managedTypeIdentity(*F.getParent()->getFunction("llam_managed_native_leave")),
                            LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL, LLAM_LLPM2_CALL_NATIVE_THUNK |
                            LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_GUARD_LEAVE});
                        BoundaryPhase = 3U;
                    } else report_fatal_error("LLAM foreign boundary does not have balanced native guards");
                    Effects |= 1U << LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL;
                }
            }
        }
        MachineBasicBlock &Last = MF.back();
        if (LastBodyInstruction != nullptr) {
            End = LastBodyInstruction->getPostInstrSymbol();
            if (End == nullptr) {
                End = Context.createNamedTempSymbol("llam_end");
                LastBodyInstruction->setPostInstrSymbol(MF, End);
            }
        }
        if (End == nullptr) report_fatal_error("LLAM empty managed function");
        if ((EntryFlags & LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE) != 0U && RuntimeCalls != 1U) {
            report_fatal_error("LLAM runtime bridge lacks its SDK call witness");
        }
        if ((EntryFlags & LLAM_LLPM2_ENTRY_NATIVE_BOUNDARY) != 0U &&
            BoundaryPhase != ((EntryFlags & LLAM_LLPM2_ENTRY_NORETURN) != 0U ? 2U : 3U)) {
            report_fatal_error("LLAM incomplete guarded foreign boundary");
        }

        MCSymbol *NativeEnd = nullptr;
        StringRef NativeName = F.getFnAttribute("llam.managed.native").getValueAsString();
        StringRef NativeIdentity = F.hasFnAttribute("llam.managed.identity")
            ? F.getFnAttribute("llam.managed.identity").getValueAsString() : NativeName;
        Function *NativeFunction = F.getParent()->getFunction(NativeName);
        if (NativeFunction != nullptr && NativeFunction != &F) {
            MachineFunction *NativeMF = getAnalysis<MachineModuleInfoWrapperPass>().getMMI().getMachineFunction(*NativeFunction);
            if (NativeMF == nullptr || Bindings == nullptr) report_fatal_error("LLAM native thunk is missing");
            materializeWindowsCallPadding(*NativeMF, TII);
            unsigned Phase = 0U;
            unsigned InstructionCount = 0U;
            MachineInstr *LastNativeInstruction = nullptr;
            const std::string BindingSymbol = symbolName(Bindings->getName(), MF.getDataLayout());
            for (MachineBasicBlock &BB : *NativeMF) {
                for (MachineInstr &MI : BB) {
                    if (MI.isMetaInstruction()) continue;
                    LastNativeInstruction = &MI;
                    if (++InstructionCount > 4096U || MI.isInlineAsm()) {
                        report_fatal_error("LLAM native thunk exceeds its qualified instruction profile");
                    }
                    if (MI.isCall()) {
                        if (MI.getPreInstrSymbol() != nullptr || MI.getPostInstrSymbol() != nullptr) {
                            report_fatal_error("LLAM native call witness already exists");
                        }
                        MCSymbol *Before = Context.createNamedTempSymbol("llam_native_call");
                        MCSymbol *After = Context.createNamedTempSymbol("llam_native_call_end");
                        MI.setPreInstrSymbol(*NativeMF, Before);
                        MI.setPostInstrSymbol(*NativeMF, After);
                        int Guard = guardCall(MI, Bindings, TRI, TII, MF.getDataLayout());
                        const Function *Target = nullptr;
                        for (const MachineOperand &O : MI.operands()) {
                            if (O.isGlobal()) Target = dyn_cast<Function>(O.getGlobal());
                        }
                        if (Guard == 0 && Phase == 0U) {
                            Calls.push_back({Before, After, BindingSymbol,
                                llam::managedTypeIdentity(*F.getParent()->getFunction("llam_managed_native_enter")),
                                LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL, LLAM_LLPM2_CALL_NATIVE_THUNK |
                                LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_GUARD_ENTER});
                            Phase = 1U;
                        } else if (Target == &F && Phase == 1U) {
                            Calls.push_back({Before, After, symbolName(NativeIdentity, MF.getDataLayout()),
                                number(F, "llam.managed.type"), LLAM_LLPM2_EFFECT_MANAGED, LLAM_LLPM2_CALL_NATIVE_THUNK});
                            Phase = 2U;
                        } else if (Guard == 1 && Phase == 2U && (EntryFlags & LLAM_LLPM2_ENTRY_NORETURN) == 0U) {
                            Calls.push_back({Before, After, BindingSymbol + " + 8",
                                llam::managedTypeIdentity(*F.getParent()->getFunction("llam_managed_native_leave")),
                                LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL, LLAM_LLPM2_CALL_NATIVE_THUNK |
                                LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_GUARD_LEAVE});
                            Phase = 3U;
                        } else {
                            report_fatal_error(Twine("LLAM native thunk call/guard is unqualified: ") + NativeName);
                        }
                        Effects |= 1U << LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL;
                    }
                }
            }
            if (LastNativeInstruction != nullptr) {
                NativeEnd = LastNativeInstruction->getPostInstrSymbol();
                if (NativeEnd == nullptr) {
                    NativeEnd = Context.createNamedTempSymbol("llam_native_end");
                    LastNativeInstruction->setPostInstrSymbol(*NativeMF, NativeEnd);
                }
            }
            if (NativeEnd == nullptr || Phase != ((EntryFlags & LLAM_LLPM2_ENTRY_NORETURN) != 0U ? 2U : 3U)) {
                report_fatal_error("LLAM native thunk lacks balanced entry/return witnesses");
            }
        }

        std::string Assembly;
        raw_string_ostream OS(Assembly);
        const bool EmitAnchor = !AnchorEmitted;
        StringRef Build = F.getFnAttribute("llam.managed.build").getValueAsString();
        if (Build.size() != 32U) report_fatal_error("LLAM invalid build identity");
        std::string Anchor = symbolName("__llam_managed_anchor_" + Build, MF.getDataLayout());
        if (T.isOSDarwin()) OS << ".section __TEXT,__llam_managed,regular\n";
        else if (T.isOSWindows()) OS << ".section .llamm,\"dr\"\n";
        else OS << ".pushsection .llam.managed,\"a\",@progbits\n";
        OS << ".p2align 3\n";
        if (EmitAnchor) {
            OS << ".globl " << Anchor << '\n' << Anchor << ":\n";
            if (!T.isOSDarwin() && !T.isOSWindows()) OS << ".type " << Anchor << ",@object\n";
            AnchorEmitted = true;
        }
        OS << ".long " << LLAM_PREEMPT_METADATA_MAGIC << "\n.short 2,128\n";
        uint64_t Total = LLAM_LLPM2_HEADER_BYTES + LLAM_LLPM2_ENTRY_BYTES +
            Ranges.size() * LLAM_LLPM2_RANGE_BYTES + Calls.size() * LLAM_LLPM2_CALL_BYTES;
        OS << ".quad " << Total << "\n.long 1," << Ranges.size() << ',' << Calls.size()
           << "\n.short 96,32,48," << (Arm ? 2 : 1) << ','
           << (T.isOSDarwin() ? 2 : (T.isOSWindows() ? 3 : 1))
           << ",1,0,2,2,1\n.long "
           << ((std::getenv("LLAM_MANAGED_REQUIRED") != nullptr ? LLAM_LLPM2_REQUIRED : 0U) |
               LLAM_LLPM2_MACHINE_VERIFIED | LLAM_LLPM2_IMMUTABLE_TEXT | LLAM_LLPM2_IMAGE_ANCHOR |
               (Bindings != nullptr ? LLAM_LLPM2_GUARD_BINDINGS : 0U)) << ','
           << (T.isOSWindows() ? 1 : 0) << "\n.byte ";
        for (unsigned I = 0U; I < 16U; ++I) {
            unsigned Byte;
            if (Build.substr(I * 2U, 2U).getAsInteger(16, Byte)) report_fatal_error("LLAM invalid build identity");
            if (I != 0U) OS << ',';
            OS << Byte;
        }
        OS << "\n.quad " << LLAM_LLPM2_PROFILE_LLVM23 << ",128,224,"
           << (224U + Ranges.size() * LLAM_LLPM2_RANGE_BYTES) << ",0\n.byte ";
        for (unsigned I = 0U; I < 16U; ++I) {
            unsigned Byte;
            if (Build.substr(I * 2U, 2U).getAsInteger(16, Byte)) report_fatal_error("LLAM invalid anchor identity");
            if (I != 0U) OS << ',';
            OS << Byte;
        }
        OS << '\n';
        std::string Native = symbolName(NativeIdentity, MF.getDataLayout());
        std::string Body = Class == LLAM_LLPM2_LEGACY && F.hasFnAttribute("llam.managed.identity")
            ? Native : symbolName(F.getName(), MF.getDataLayout());
        // COFF has no signed PC-relative 64-bit relocation. The unsealed object
        // uses RVA32 + zero high word; the final-link verifier converts each
        // field to canonical relative64 before granting LINK_VERIFIED. RVA
        // relocations create no loader rebases into the sealed metadata.
        auto Address = [&](StringRef Name) {
            if (T.isOSWindows()) OS << ".rva " << Name << "\n.long 0\n";
            else OS << ".quad " << Name << " - .\n";
        };
        Address(Native);
        if (Class == LLAM_LLPM2_LEGACY) OS << ".quad 0\n";
        else Address(Body);
        Address(Body);
        Address(End->getName());
        OS << ".long " << Class << ',' << Effects << "\n.quad " << ISA << "\n.long 1,0," << Ranges.size()
           << ",0," << Calls.size() << ',' << EntryFlags << "\n.quad "
           << number(F, "llam.managed.type") << '\n';
        if (Bindings != nullptr) Address(symbolName(Bindings->getName(), MF.getDataLayout()));
        else OS << ".quad 0\n";
        if (NativeEnd != nullptr) Address(NativeEnd->getName());
        else OS << ".quad 0\n";
        for (const Interval &Range : Ranges) {
            OS << ".long 0\n.short " << Range.Kind << ",0\n";
            Address(Range.Begin->getName());
            Address(Range.End->getName());
            OS << ".long " << (Arm ? 4 : 3) << ",0\n";
        }
        for (const CallRecord &Call : Calls) {
            OS << ".long 0\n.short " << Call.Effect << ',' << Call.Flags << "\n";
            Address(Call.Begin->getName());
            Address(Call.End->getName());
            if (Call.Target.empty()) OS << ".quad 0\n";
            else Address(Call.Target);
            OS << ".quad " << Call.Type << ',' << Call.Contract << '\n';
        }
        if (!T.isOSDarwin() && !T.isOSWindows()) {
            const Function *LastRecord = nullptr;
            for (const Function &Candidate : *F.getParent()) {
                if (!Candidate.isDeclaration() && Candidate.hasFnAttribute("llam.managed.class")) LastRecord = &Candidate;
            }
            if (LastRecord == &F) OS << ".size " << Anchor << ", . - " << Anchor << '\n';
        }
        if (T.isOSDarwin()) OS << ".section __TEXT,__text,regular,pure_instructions\n";
        else if (T.isOSWindows()) {
            if (EmitAnchor) OS << ".section .drectve,\"yn\"\n.ascii \" /EXPORT:" << Anchor << ",DATA\"\n";
            OS << ".text\n";
        }
        else OS << ".popsection\n";
        // Section directives emit no text. Keep the carrier before terminators
        // so the labelled MIR remains valid even with machine verification on.
        BuildMI(Last, Last.begin(), DebugLoc(), TII.get(TargetOpcode::INLINEASM))
            .addExternalSymbol(MF.createExternalSymbolName(Assembly))
            .addImm(InlineAsm::Extra_HasSideEffects);
        return true;
    }
};
char ManagedMachinePass::ID = 0;
RegisterPass<ManagedMachinePass> Registration("llam-managed-machine",
    "LLAM final physical instruction and entry certificate", false, false);
} // namespace
