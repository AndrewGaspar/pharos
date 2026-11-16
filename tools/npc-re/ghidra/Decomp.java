// Decompile functions at the given RVAs (hex, script args) of the raw image dump.
// Usage (see decomp.sh): analyzeHeadless <proj> aco -process img -noanalysis -scriptPath <dir> -postScript Decomp.java <rva>...
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class Decomp extends GhidraScript {
  @Override
  public void run() throws Exception {
    Address base = currentProgram.getMinAddress();
    PrintWriter out = new PrintWriter(System.getenv("DECOMP_OUT"));
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    for (String a : getScriptArgs()) {
      Address addr = base.add(Long.parseLong(a.replace("0x", ""), 16));
      Function fn = getFunctionContaining(addr);
      if (fn == null || !fn.getEntryPoint().equals(addr)) {
        disassemble(addr);
        new CreateFunctionCmd(addr).applyTo(currentProgram);
        fn = getFunctionAt(addr);
      }
      if (fn == null) { out.println("// no function at " + a); continue; }
      DecompileResults r = di.decompileFunction(fn, 120, monitor);
      out.println("// ==== rva " + a + " (" + fn.getBody().getNumAddresses() + " bytes)");
      out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
    }
    out.close();
  }
}
