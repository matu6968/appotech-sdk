import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.CodeUnit;
public class AnnotateSD extends GhidraScript {
 public void run() throws Exception {
  long[] addresses={0x02050a4c,0x02050c0c,0x02050d58,0x02050d9c,
   0x02050eb8,0x02050f2c,0x02050f48,0x02050fa8,0x020510a8,
   0x02032478,0x020322dc,0x02022d14};
  String[] names={"sd_controller_init","sd_command","sd_bus_width","sd_receive_dma_arm",
   "sd_receive_dma_wait","sd_short_response","sd_data_status","sd_long_response_sram",
   "sd_clock_select","sd_card_identify","sd_sector_transfer","ddr_receive_dma_maintenance"};
  for(int i=0;i<addresses.length;++i) {
   createLabel(toAddr(addresses[i]),"jt5295_"+names[i],true);
   currentProgram.getListing().setComment(toAddr(addresses[i]),CodeUnit.PRE_COMMENT,
    "JT5295 SD evidence: "+names[i]+". Controller SPR 9880..98a4, CMD 9898, argument/response 989c, DMA 9890/9894, R2 SRAM 2000. See appotech-sdk/research/sd.md. Raw native LE instructions take precedence over generic OpenRISC delay-slot decompilation.");
   println(String.format("%08x %s",addresses[i],names[i]));
  }
 }
}
