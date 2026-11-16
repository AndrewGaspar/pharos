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
      boolean inner = a.startsWith("@");  // "@rva": an address inside a function; find its start
      Address addr = base.add(Long.parseLong(a.replace("@", "").replace("0x", ""), 16));
      if (inner) {
        // MSVC pads between functions with int3 (0xCC): walk back to the first byte after padding.
        Address p = addr;
        for (int i = 0; i < 0x8000; i++) {
          if (getByte(p.subtract(1)) == (byte) 0xcc && getByte(p.subtract(2)) == (byte) 0xcc) break;
          p = p.subtract(1);
        }
        addr = p;
        a = Long.toHexString(addr.subtract(base)) + " (containing " + a + ")";
      }
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
