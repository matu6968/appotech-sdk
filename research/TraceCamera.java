import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
public class TraceCamera extends GhidraScript {
 public void run() throws Exception {
  DecompInterface d=new DecompInterface();d.openProgram(currentProgram);
  long[] starts={0x02023874L,0x020280f4L,0x0202b4b8L,0x02035ed8L,0x020354d4L,0x0203618cL,0x02036320L,0x02042f14L,0x02043618L};
  for(long a:starts){Function f=getFunctionAt(toAddr(a));
   println("ADDRESS "+Long.toHexString(a)+" function "+f);
   if(f!=null){DecompileResults r=d.decompileFunction(f,20,monitor);if(r.decompileCompleted())println(r.getDecompiledFunction().getC());}
  }d.dispose();
 }
}
