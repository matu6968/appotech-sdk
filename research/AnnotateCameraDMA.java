import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.SourceType;
public class AnnotateCameraDMA extends GhidraScript {
 public void run() throws Exception {
  long[] a={0x0202b3d8L,0x0202b474L,0x0202b4b8L,0x0202b930L,0x0202fb34L,0x0202fe88L};
  String[] n={"Camera_scaled_DMA_arm","Camera_scaled_DMA_rearm_if_enabled","Camera_scaled_crop_filter_pitch",
   "Camera_scaler_update_ready","Camera_preview_scaled_start","Camera_scaled_frame_complete_handoff"};
  for(int i=0;i<a.length;++i){createLabel(toAddr(a[i]),n[i],true,SourceType.USER_DEFINED);println(Long.toHexString(a[i])+" "+n[i]);}
  setPlateComment(toAddr(0x0202b4b8L),"Verified live 2026-10-15: camera scaler uses b004 bits17/22, crop b030/b034, output b180, ratios b184/b188 = (input<<16)/(output/2), 24 filter words b18c..b1e8 from panel resource SPI96104, byte pitch b068. Output planes b060/b064, NV21. SDK 220x176 pitch224 preview leaves sensor running and uses two guarded DMA buffers.");
  setPlateComment(toAddr(0x0202b3d8L),"Verified live: b00c bit1 is scaled frame completion; bit3 is frame start. b018 bit18 is one-shot scaled output DMA and self-clears at completion. b018 bit21 is stock software rearm policy checked by 0202b474. CPU must rearm only a free buffer after LCD release; SDK polling masks PIC14. Do not confuse native b00c bit0 / b018 bit19.");
  println("Camera DMA evidence applied; raw JT5295 instructions authoritative over generic OpenRISC delay-slot analysis.");
 }
}
