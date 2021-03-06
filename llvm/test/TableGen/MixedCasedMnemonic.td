// RUN: llvm-tblgen -gen-asm-matcher -I %p/../../include %s | FileCheck %s --check-prefix=MATCHER
// RUN: llvm-tblgen -gen-asm-writer -I %p/../../include %s | FileCheck %s --check-prefix=WRITER

// Check that an instruction that uses mixed upper/lower case in its mnemonic
// is printed as-is, and is parsed in its "canonicalized" lowercase form.

include "llvm/Target/Target.td"

def ArchInstrInfo : InstrInfo { }

def Arch : Target {
  let InstructionSet = ArchInstrInfo;
}

def Reg : Register<"reg">;
def RegClass : RegisterClass<"foo", [i32], 0, (add Reg)>;

// Define instructions that demonstrate case-insensitivity.
// In case-sensitive ASCII order, "BInst" < "aInst".
// In case-insensitive order, "aInst" < "BInst".
// If the matcher really treats the mnemonics in a case-insensitive way,
// then we should see "aInst" appearing before "BInst", despite the
// fact that "BInst" would appear before "aInst" in ASCIIbetical order.
def AlphabeticallySecondInst : Instruction {
  let Size = 2;
  let OutOperandList = (outs);
  let InOperandList = (ins);
  let AsmString = "BInst";
}

def AlphabeticallyFirstInst : Instruction {
  let Size = 2;
  let OutOperandList = (outs);
  let InOperandList = (ins);
  let AsmString = "aInst";
}

// Check that the matcher lower()s the mnemonics it matches.
// MATCHER: static const char *const MnemonicTable =
// MATCHER-NEXT: "\005ainst\005binst";

// Check that aInst appears before BInst in the match table.
// This shows that the mnemonics are sorted in a case-insensitive way,
// since otherwise "B" would be less than "a" by ASCII order.
// MATCHER:      static const MatchEntry MatchTable0[] = {
// MATCHER-NEXT:     /* aInst */, ::AlphabeticallyFirstInst
// MATCHER-NEXT:     /* BInst */, ::AlphabeticallySecondInst
// MATCHER-NEXT: };

// Check that the writer preserves the case of the mnemonics.
// WRITER:      static const char AsmStrs[] = {
// WRITER:        "BInst\0"
// WRITER-NEXT:   "aInst\0"
// WRITER-NEXT: };

