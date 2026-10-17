import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
public class AnnotateJPEG extends GhidraScript {
 public void run() throws Exception {
  /* The initial generic-OpenRISC trace created false function boundaries.
   * Remove only its auto-named functions; verified starts receive labels. */
  long[] initial={0x202ccd0,0x202cf40,0x202cf8c,0x202d16c,0x202d3f0,0x202d628,0x202d7f0,0x202d940,0x202d97c,0x202da08,0x202dd24,0x202e054,0x202e730};
  for(long a:initial){Function f=getFunctionAt(toAddr(a));if(f!=null&&f.getName().startsWith("FUN_"))currentProgram.getFunctionManager().removeFunction(toAddr(a));}
  long[] addresses={0x202cc10,0x202ccd0,0x202cf40,0x202d16c,0x202d3d4,0x202d3f0,0x202d628,0x202d694,0x202d764,0x202d988,0x202d9a4,0x202da0c,0x202da50,0x202dcf4,0x202ddc8,0x202df60,0x202e058,0x202e30c,0x202e328,0x202e3bc,0x202e608,0x2037318,0x20377f0};
  String[] names={"JPEG_shared_clock_reset","JPEG_b800_wait","JPEG_b800_output_bounds","JPEG_b800_geometry","JPEG_b800_nv21_sources","JPEG_b800_initialize","JPEG_b800_start","JPEG_decoder_horizontal_prescale","JPEG_decoder_geometry","JPEG_decoder_nv21_output","JPEG_decoder_entropy_input","JPEG_decoder_load_tables","JPEG_decoder_initialize","JPEG_decoder_start","JPEG_decoder_stop_reset","JPEG_decoder_busy","JPEG_c000_encoder_initialize","JPEG_c000_encoder_nv21_sources","JPEG_c000_encoder_output_bounds","JPEG_c000_encoder_start","JPEG_decoder_wait","JPEG_parse_baseline_SOF0","JPEG_configure_from_header"};
  for(int i=0;i<addresses.length;++i){createLabel(toAddr(addresses[i]),names[i],true,SourceType.USER_DEFINED);
   setPlateComment(toAddr(addresses[i]),"JT5295 JPEG research: raw native little-endian/no-delay disassembly is authoritative. SDK research/jpeg.md. Labels identify traced entry sequences; generic OpenRISC function boundaries/decompilation are unverified.");
   println(Long.toHexString(addresses[i])+" "+names[i]);}
  setEOLComment(toAddr(0x202d694),"Horizontal prescaler: initial reduction above 1920 pixels, additional powers of two for reduced_width/(target_width*4), then 16.16 ratio.");
  setEOLComment(toAddr(0x202da50),"Decoder uses SPR c000..c148, tables at 66400. 420 selects c000=60000023; 422 sets bit16. Output is NV21, c0c0 low byte carries row pitch.");
  setEOLComment(toAddr(0x202e058),"Shared core also encodes: input Y/VU at c020/c024, bounded JPEG output c010/c014, result end at c01c. Packed AC Huffman code=(length-1)<<12 | (code&fff).");
  println("JPEG labels/comments applied; initial trace function boundaries removed.");
 }
}
