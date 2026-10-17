import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.CodeUnit;
public class AnnotateSDWrite extends GhidraScript {
 public void run() throws Exception {
  long[] addresses={0x02050df0,0x02050e28,0x02050f48,0x020321bc,0x02032258,0x020322dc,0x02022c98};
  String[] names={"sd_transmit_dma_arm","sd_wait_dat0_ready","sd_receive_transmit_status",
   "sd_wait_transfer_state","sd_stop_multiblock","sd_read_write_sector","ddr_transmit_cache_clean"};
  for(int i=0;i<addresses.length;++i) {
   createLabel(toAddr(addresses[i]),"jt5295_"+names[i],true);
   currentProgram.getListing().setComment(toAddr(addresses[i]),CodeUnit.PRE_COMMENT,
    "SD write evidence: "+names[i]+". TX arm: 9890 source, 9884 W1C bit15, 9894 length, event bits 4/5 (0x30). DATA_DONE 9880 bit15, TX error bit16. Stock busy wait sets CONTROL bit1 and waits bit20, then clears bit1 and polls CMD13 transfer state. Source cache maintenance 181c/180c. SDK implements single-block CMD24 independently; first native sector write/readback/restoration passed 2026-10-14. See appotech-sdk/research/sd-write.md and sd-write-vendor-raw.disasm. Raw LE/no-delay instructions override generic OpenRISC decompilation.");
   println(String.format("%08x %s",addresses[i],names[i]));
  }
 }
}
