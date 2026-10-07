// Export the functions Ghidra's auto-analysis found, as "ENTRY<TAB>SIZE<TAB>MODE"
// lines (MODE = T if the TMode context register is 1 at the entry, else A), to
// compare with ndskit's function discovery on the same program.
//
// The same script runs twice: as a preScript ("root ADDR [aggressive]") it marks
// ADDR as ARM code, disassembles it and creates a function there so analysis has
// a root (and optionally enables the ARM Aggressive Instruction Finder); as a
// postScript ("OUT.tsv [ADDR]") it re-creates the root function if it is missing
// and writes the list.
//
//   set JAVA_HOME=<JDK 21>
//   analyzeHeadless PROJDIR NAME -import arm9.bin -overwrite \
//       -processor ARM:LE:32:v5t -loader BinaryLoader -loader-baseAddr 0x02000000 \
//       -scriptPath ndskit/ghidra \
//       -preScript ExportFuncs.java root 0x02000800 \
//       -postScript ExportFuncs.java OUT.tsv 0x02000800
//
// ARM7: -processor ARM:LE:32:v4t -loader-baseAddr 0x02380000, root 0x02380000.
// Add "aggressive" after the preScript address to enable the ARM Aggressive
// Instruction Finder analyzer.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import java.io.*;
import java.math.BigInteger;

public class ExportFuncs extends GhidraScript {
    private void makeArmRoot(Address a) throws Exception {
        if (getFunctionAt(a) != null) {
            return;
        }
        Register tmode = currentProgram.getRegister("TMode");
        if (getInstructionAt(a) == null) {
            if (tmode != null) {
                currentProgram.getProgramContext().setValue(tmode, a, a, BigInteger.ZERO);
            }
            disassemble(a);
        }
        createFunction(a, null);
        addEntryPoint(a);
    }

    private boolean isThumb(Address a) {
        Register tmode = currentProgram.getRegister("TMode");
        if (tmode == null) {
            return false;
        }
        BigInteger v = currentProgram.getProgramContext().getValue(tmode, a, false);
        return v != null && v.signum() != 0;
    }

    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length >= 2 && args[0].equals("root")) {
            makeArmRoot(toAddr(args[1]));
            if (args.length >= 3 && args[2].equals("aggressive")) {
                setAnalysisOption(currentProgram, "ARM Aggressive Instruction Finder", "true");
            }
            return;
        }
        if (args.length >= 2) {
            makeArmRoot(toAddr(args[1]));
        }
        PrintWriter w = new PrintWriter(new FileWriter(args[0]));
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            Address e = f.getEntryPoint();
            w.println(String.format("%08X\t%d\t%s", e.getOffset(),
                                    f.getBody().getNumAddresses(), isThumb(e) ? "T" : "A"));
        }
        w.close();
    }
}
