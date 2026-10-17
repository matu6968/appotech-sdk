import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.SourceType;
public class AnnotateMicrophone extends GhidraScript {
 public void run() throws Exception {
  long[] a={0x2027c24L,0x2027cf4L,0x2027decL,0x2027e7cL,0x2027ef8L,0x2027f58L,0x2027ffcL,0x2034d40L,0x2034fc0L,0x20351f0L};
  String[] n={"Microphone_IRQ_half_full","Microphone_enable_FIFO_reset","Microphone_analog_AGC_setup","Microphone_gain_parameters","Microphone_DMA_destination_words","Microphone_shared_clock_init","Microphone_stock_rate_prescaler","Microphone_service_initialize","Microphone_buffered_record_start","Microphone_record_stop"};
  for(int i=0;i<a.length;++i){createLabel(toAddr(a[i]),n[i],true,SourceType.USER_DEFINED);println(Long.toHexString(a[i])+" "+n[i]);}
  setPlateComment(toAddr(0x2027c24L),"JT5295 recording: SPR9114 bit1 is half-buffer (stock next-buffer callback), bit0 is complete-buffer notification. SDK waits for bit0; early bit1 completion leaves half the destination untouched. PIC IRQ18 remains masked in polling SDK.");
  setPlateComment(toAddr(0x2027ef8L),"Recording DMA destination SPR9108, count SPR910c=bytes/4-1. Stock invalidates CPU destination cache with immediate SPR181c/1814 before DMA. SDK also invalidates after quiescing completed DMA.");
  setPlateComment(toAddr(0x2027ffcL),"Stock rate settings do not match inherited SDK audio clock. Verified SDK PCM16 mono controls: 9100=001f4000/000f4000/00074000 at 8/16/32kHz with shared 884c divider b unchanged. Exact clock-tree/field naming remains inferred. See research/microphone.md and native timing tests.");
  println("Microphone labels/comments applied; generic delay-slot function bodies left untouched.");
 }
}
