import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.listing.Function;
import ghidra.app.decompiler.*;

public class TraceSD extends GhidraScript {
    public void run() throws Exception {
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        for (long a : new long[]{0x02000180,0x02029594,0x02029684,0x02029700,0x02029800,0x02029904}) {
            println("STRING " + toAddr(a));
            for (Reference r : currentProgram.getReferenceManager().getReferencesTo(toAddr(a))) {
                println(r.toString());
            }
                Function f = getFunctionContaining(toAddr(a));
                if (f != null) {
                    println("FUNCTION " + f.getEntryPoint());
                    DecompileResults result = d.decompileFunction(f,30,monitor);
                    if(result.decompileCompleted()) println(result.getDecompiledFunction().getC());
                }
        }
        d.dispose();
    }
}
