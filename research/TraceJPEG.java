import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.app.decompiler.*;
public class TraceJPEG extends GhidraScript {
 public void run() throws Exception {
  DecompInterface d=new DecompInterface();d.openProgram(currentProgram);
  /* Read-only inventory. Apply verified labels with AnnotateJPEG instead of
   * creating functions from generic OpenRISC delay-slot analysis. */
  FunctionIterator it=currentProgram.getFunctionManager().getFunctions(toAddr(0x0202cc00),true);
  while(it.hasNext()){Function f=it.next();if(f.getEntryPoint().getOffset()>0x0202e800)break;
   println("FUNCTION "+f.getEntryPoint()+" "+f.getName()+" "+f.getBody().getMaxAddress());
   DecompileResults r=d.decompileFunction(f,20,monitor);if(r.decompileCompleted())println(r.getDecompiledFunction().getC());
  }d.dispose();
 }
}
