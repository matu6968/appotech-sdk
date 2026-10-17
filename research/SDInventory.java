import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.listing.FunctionIterator;
public class SDInventory extends GhidraScript {
 public void run() throws Exception {
  for(MemoryBlock b:currentProgram.getMemory().getBlocks()) println(b.toString());
  FunctionIterator i=currentProgram.getFunctionManager().getFunctions(true);
  int n=0;while(i.hasNext()&&n++<30)println(i.next().toString());
  println("FUNCTION COUNT "+currentProgram.getFunctionManager().getFunctionCount());
 }
}
