import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.listing.Function;
public class TraceWatchdogPower extends GhidraScript {
 public void run() throws Exception {
  for(long value:new long[]{0x020237dc,0x02023824,0x02023858,0x020150e4,0x02013fb0,0x0205c154}) {
   println("TARGET "+toAddr(value));
   Function f=getFunctionContaining(toAddr(value));
   if(f!=null)println("FUNCTION "+f.getEntryPoint()+" "+f.getName());
   for(Reference r:currentProgram.getReferenceManager().getReferencesTo(toAddr(value)))println(r.toString());
  }
 }
}
