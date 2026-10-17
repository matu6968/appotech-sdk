import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;

public class USBTrace extends GhidraScript {
    public void run() throws Exception {
        println("PROGRAM " + currentProgram.getName());
        for (long address = 0x106794; address < 0x106880; address += 4) {
            println(String.format("TABLE %08x %08x", address,
                currentProgram.getMemory().getInt(toAddr(address))));
        }
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        long[] entries = {0x104ffc, 0x105018, 0x10508c, 0x105110,
                          0x1051ac, 0x105244, 0x1053d8, 0x105768,
                          0x1058b4, 0x1059b4, 0x105bb8, 0x105c68};
        for (long address : entries) {
            Function function = currentProgram.getFunctionManager().getFunctionAt(toAddr(address));
            println("FUNCTION " + Long.toHexString(address));
            if (function != null) {
                DecompileResults result = decompiler.decompileFunction(function, 15, monitor);
                if (result.decompileCompleted()) println(result.getDecompiledFunction().getC());
            } else println("Not defined");
        }
        decompiler.dispose();
    }
}
