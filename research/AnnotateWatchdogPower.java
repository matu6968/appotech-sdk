import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.CodeUnit;
public class AnnotateWatchdogPower extends GhidraScript {
 public void run() throws Exception {
  long[] addresses={0x020237dc,0x02023824,0x02023858,0x02025f44,0x02025f94,
                    0x02026130,0x020261b8,0x02026624,0x02026980,0x020269d0,0x02026a0c,0x0200acb4};
  String[] names={"watchdog_enable_disable","watchdog_conditional_feed","watchdog_force_reset",
                  "power_bus_byte_out","power_bus_byte_in","power_bank_read","power_bank_write",
                  "power_pm41_control","power_pm45_control","power_pm43_e0","power_pm55_bit5","board_shutdown"};
  for(int i=0;i<addresses.length;i++) {
   createLabel(toAddr(addresses[i]),"jt5295_"+names[i],true);
   currentProgram.getListing().setComment(toAddr(addresses[i]),CodeUnit.PRE_COMMENT,
    "JT5295 watchdog/power evidence 2026-10-14: SPR 880c, disable key 5500; 50000|selector reloads/enables (readback 18|selector); 39 requests reset. Selectors 4/5/6/7 physically expire at about .46/1.82/7.31/29.10 seconds. Always-powered serial bank: CS 88e0 bit8, busy bit3, data 88ec; even read/odd write command addresses 40..5b. Board shutdown clears PM54 bit5 through 55, writes PM45=10, PM43=e0, then PM41=02. Off acknowledged and USB detached; owner confirms subsequent Power-button boot works. Exact rail meanings remain undocumented. See appotech-sdk/research/watchdog-power.md and raw disassembly. Raw LE/no-delay instructions take precedence over generic OpenRISC decompilation.");
   println(String.format("%08x %s",addresses[i],names[i]));
  }
 }
}
