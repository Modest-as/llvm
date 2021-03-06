//===-- Uops.cpp ------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Uops.h"

#include "Assembler.h"
#include "BenchmarkRunner.h"
#include "MCInstrDescView.h"
#include "Target.h"

// FIXME: Load constants into registers (e.g. with fld1) to not break
// instructions like x87.

// Ideally we would like the only limitation on executing uops to be the issue
// ports. Maximizing port pressure increases the likelihood that the load is
// distributed evenly across possible ports.

// To achieve that, one approach is to generate instructions that do not have
// data dependencies between them.
//
// For some instructions, this is trivial:
//    mov rax, qword ptr [rsi]
//    mov rax, qword ptr [rsi]
//    mov rax, qword ptr [rsi]
//    mov rax, qword ptr [rsi]
// For the above snippet, haswell just renames rax four times and executes the
// four instructions two at a time on P23 and P0126.
//
// For some instructions, we just need to make sure that the source is
// different from the destination. For example, IDIV8r reads from GPR and
// writes to AX. We just need to ensure that the Var is assigned a
// register which is different from AX:
//    idiv bx
//    idiv bx
//    idiv bx
//    idiv bx
// The above snippet will be able to fully saturate the ports, while the same
// with ax would issue one uop every `latency(IDIV8r)` cycles.
//
// Some instructions make this harder because they both read and write from
// the same register:
//    inc rax
//    inc rax
//    inc rax
//    inc rax
// This has a data dependency from each instruction to the next, limit the
// number of instructions that can be issued in parallel.
// It turns out that this is not a big issue on recent Intel CPUs because they
// have heuristics to balance port pressure. In the snippet above, subsequent
// instructions will end up evenly distributed on {P0,P1,P5,P6}, but some CPUs
// might end up executing them all on P0 (just because they can), or try
// avoiding P5 because it's usually under high pressure from vector
// instructions.
// This issue is even more important for high-latency instructions because
// they increase the idle time of the CPU, e.g. :
//    imul rax, rbx
//    imul rax, rbx
//    imul rax, rbx
//    imul rax, rbx
//
// To avoid that, we do the renaming statically by generating as many
// independent exclusive assignments as possible (until all possible registers
// are exhausted) e.g.:
//    imul rax, rbx
//    imul rcx, rbx
//    imul rdx, rbx
//    imul r8,  rbx
//
// Some instruction even make the above static renaming impossible because
// they implicitly read and write from the same operand, e.g. ADC16rr reads
// and writes from EFLAGS.
// In that case we just use a greedy register assignment and hope for the
// best.

namespace exegesis {

static llvm::SmallVector<const Variable *, 8>
getVariablesWithTiedOperands(const Instruction &Instr) {
  llvm::SmallVector<const Variable *, 8> Result;
  for (const auto &Var : Instr.Variables)
    if (Var.hasTiedOperands())
      Result.push_back(&Var);
  return Result;
}

static void remove(llvm::BitVector &a, const llvm::BitVector &b) {
  assert(a.size() == b.size());
  for (auto I : b.set_bits())
    a.reset(I);
}

UopsBenchmarkRunner::~UopsBenchmarkRunner() = default;

UopsSnippetGenerator::~UopsSnippetGenerator() = default;

void UopsSnippetGenerator::instantiateMemoryOperands(
    const unsigned ScratchSpacePointerInReg,
    std::vector<InstructionTemplate> &Instructions) const {
  if (ScratchSpacePointerInReg == 0)
    return; // no memory operands.
  const auto &ET = State.getExegesisTarget();
  const unsigned MemStep = ET.getMaxMemoryAccessSize();
  const size_t OriginalInstructionsSize = Instructions.size();
  size_t I = 0;
  for (InstructionTemplate &IT : Instructions) {
    ET.fillMemoryOperands(IT, ScratchSpacePointerInReg, I * MemStep);
    ++I;
  }

  while (Instructions.size() < kMinNumDifferentAddresses) {
    InstructionTemplate IT = Instructions[I % OriginalInstructionsSize];
    ET.fillMemoryOperands(IT, ScratchSpacePointerInReg, I * MemStep);
    ++I;
    Instructions.push_back(std::move(IT));
  }
  assert(I * MemStep < BenchmarkRunner::ScratchSpace::kSize &&
         "not enough scratch space");
}

llvm::Expected<std::vector<CodeTemplate>>
UopsSnippetGenerator::generateCodeTemplates(const Instruction &Instr) const {
  CodeTemplate CT;
  const llvm::BitVector *ScratchSpaceAliasedRegs = nullptr;
  if (Instr.hasMemoryOperands()) {
    const auto &ET = State.getExegesisTarget();
    CT.ScratchSpacePointerInReg =
        ET.getScratchMemoryRegister(State.getTargetMachine().getTargetTriple());
    if (CT.ScratchSpacePointerInReg == 0)
      return llvm::make_error<BenchmarkFailure>(
          "Infeasible : target does not support memory instructions");
    ScratchSpaceAliasedRegs =
        &State.getRATC().getRegister(CT.ScratchSpacePointerInReg).aliasedBits();
    // If the instruction implicitly writes to ScratchSpacePointerInReg , abort.
    // FIXME: We could make a copy of the scratch register.
    for (const auto &Op : Instr.Operands) {
      if (Op.isDef() && Op.isImplicitReg() &&
          ScratchSpaceAliasedRegs->test(Op.getImplicitReg()))
        return llvm::make_error<BenchmarkFailure>(
            "Infeasible : memory instruction uses scratch memory register");
    }
  }

  const AliasingConfigurations SelfAliasing(Instr, Instr);
  InstructionTemplate IT(Instr);
  if (SelfAliasing.empty()) {
    CT.Info = "instruction is parallel, repeating a random one.";
    CT.Instructions.push_back(std::move(IT));
    instantiateMemoryOperands(CT.ScratchSpacePointerInReg, CT.Instructions);
    return getSingleton(std::move(CT));
  }
  if (SelfAliasing.hasImplicitAliasing()) {
    CT.Info = "instruction is serial, repeating a random one.";
    CT.Instructions.push_back(std::move(IT));
    instantiateMemoryOperands(CT.ScratchSpacePointerInReg, CT.Instructions);
    return getSingleton(std::move(CT));
  }
  const auto TiedVariables = getVariablesWithTiedOperands(Instr);
  if (!TiedVariables.empty()) {
    if (TiedVariables.size() > 1)
      return llvm::make_error<llvm::StringError>(
          "Infeasible : don't know how to handle several tied variables",
          llvm::inconvertibleErrorCode());
    const Variable *Var = TiedVariables.front();
    assert(Var);
    const Operand &Op = Instr.getPrimaryOperand(*Var);
    assert(Op.isReg());
    CT.Info = "instruction has tied variables using static renaming.";
    for (const llvm::MCPhysReg Reg :
         Op.getRegisterAliasing().sourceBits().set_bits()) {
      if (ScratchSpaceAliasedRegs && ScratchSpaceAliasedRegs->test(Reg))
        continue; // Do not use the scratch memory address register.
      InstructionTemplate TmpIT = IT;
      TmpIT.getValueFor(*Var) = llvm::MCOperand::createReg(Reg);
      CT.Instructions.push_back(std::move(TmpIT));
    }
    instantiateMemoryOperands(CT.ScratchSpacePointerInReg, CT.Instructions);
    return getSingleton(std::move(CT));
  }
  const auto &ReservedRegisters = State.getRATC().reservedRegisters();
  // No tied variables, we pick random values for defs.
  llvm::BitVector Defs(State.getRegInfo().getNumRegs());
  for (const auto &Op : Instr.Operands) {
    if (Op.isReg() && Op.isExplicit() && Op.isDef() && !Op.isMemory()) {
      auto PossibleRegisters = Op.getRegisterAliasing().sourceBits();
      remove(PossibleRegisters, ReservedRegisters);
      // Do not use the scratch memory address register.
      if (ScratchSpaceAliasedRegs)
        remove(PossibleRegisters, *ScratchSpaceAliasedRegs);
      assert(PossibleRegisters.any() && "No register left to choose from");
      const auto RandomReg = randomBit(PossibleRegisters);
      Defs.set(RandomReg);
      IT.getValueFor(Op) = llvm::MCOperand::createReg(RandomReg);
    }
  }
  // And pick random use values that are not reserved and don't alias with defs.
  const auto DefAliases = getAliasedBits(State.getRegInfo(), Defs);
  for (const auto &Op : Instr.Operands) {
    if (Op.isReg() && Op.isExplicit() && Op.isUse() && !Op.isMemory()) {
      auto PossibleRegisters = Op.getRegisterAliasing().sourceBits();
      remove(PossibleRegisters, ReservedRegisters);
      // Do not use the scratch memory address register.
      if (ScratchSpaceAliasedRegs)
        remove(PossibleRegisters, *ScratchSpaceAliasedRegs);
      remove(PossibleRegisters, DefAliases);
      assert(PossibleRegisters.any() && "No register left to choose from");
      const auto RandomReg = randomBit(PossibleRegisters);
      IT.getValueFor(Op) = llvm::MCOperand::createReg(RandomReg);
    }
  }
  CT.Info =
      "instruction has no tied variables picking Uses different from defs";
  CT.Instructions.push_back(std::move(IT));
  instantiateMemoryOperands(CT.ScratchSpacePointerInReg, CT.Instructions);
  return getSingleton(std::move(CT));
}

llvm::Expected<std::vector<BenchmarkMeasure>>
UopsBenchmarkRunner::runMeasurements(const FunctionExecutor &Executor) const {
  const auto &SchedModel = State.getSubtargetInfo().getSchedModel();

  std::vector<BenchmarkMeasure> Result;
  const auto &PfmCounters = SchedModel.getExtraProcessorInfo().PfmCounters;
  // Uops per port.
  for (unsigned ProcResIdx = 1;
       ProcResIdx < SchedModel.getNumProcResourceKinds(); ++ProcResIdx) {
    const char *const Counters = PfmCounters.IssueCounters[ProcResIdx];
    if (!Counters)
      continue;
    auto ExpectedCounterValue = Executor.runAndMeasure(Counters);
    if (!ExpectedCounterValue)
      return ExpectedCounterValue.takeError();
    Result.push_back(BenchmarkMeasure::Create(
        SchedModel.getProcResource(ProcResIdx)->Name, *ExpectedCounterValue));
  }
  // NumMicroOps.
  if (const char *const UopsCounter = PfmCounters.UopsCounter) {
    auto ExpectedCounterValue = Executor.runAndMeasure(UopsCounter);
    if (!ExpectedCounterValue)
      return ExpectedCounterValue.takeError();
    Result.push_back(
        BenchmarkMeasure::Create("NumMicroOps", *ExpectedCounterValue));
  }
  return std::move(Result);
}

constexpr const size_t UopsSnippetGenerator::kMinNumDifferentAddresses;

} // namespace exegesis
