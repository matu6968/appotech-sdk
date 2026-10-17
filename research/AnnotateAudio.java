import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.SourceType;
public class AnnotateAudio extends GhidraScript {
 public void run() throws Exception {
  long[] a={0x20274a4,0x20274e4,0x20275b8,0x2027628,0x20276ac,0x2027724,0x2027744,0x2027764,0x2027788,0x2027804,0x2027854,0x20278e8,0x2027958,0x20279c8,0x2027c24,0x2027cf4,0x2027dec,0x2027ef8,0x2027f58,0x2027ffc,0x202e8ec,0x202e944,0x202e9f0,0x202eb08,0x205d224,0x205e1c8};
  String[] names={"Audio_DAC_indexed_write","Audio_DAC_IRQ_dispatch","Audio_DAC_rate_selector","Audio_DAC_enable","Audio_DAC_gain","Audio_DAC_next_buffer","Audio_DAC_transfer_length","Audio_DAC_base_control","Audio_DAC_start","Audio_DAC_stop","Audio_DAC_analog_path","Audio_DAC_filter_coefficients","Audio_DAC_analog_setup","Audio_DAC_clock_initialize","Audio_recording_IRQ_dispatch","Audio_recording_start_stop","Audio_recording_analog_setup","Audio_recording_DMA_buffer","Audio_recording_clock_initialize","Audio_recording_sample_rate","Audio_playback_initialize","Audio_playback_buffer_rate_start","Audio_playback_stop_mute","Audio_playback_volume_percent","Audio_DAC_filter_table","Audio_stock_sample_rates"};
  for(int i=0;i<a.length;++i){createLabel(toAddr(a[i]),names[i],true,SourceType.USER_DEFINED);
   setPlateComment(toAddr(a[i]),"JT5295 audio: traced from raw little-endian/no-delay instructions. SDK research/audio.md. Labels mark verified sequences, not generic OpenRISC delay-slot function boundaries.");println(Long.toHexString(a[i])+" "+names[i]);}
  setEOLComment(toAddr(0x20274a4),"DAC indexed interface: 915c=(index<<24)|40000000|value, busy bit31; reads omit bit30 and return 9160. Preserve register0 enable bit when changing rate.");
  setEOLComment(toAddr(0x2027788),"Playback: 9140=202c, 9148=PCM address, 9144=bytes/4-1, 9158=bytes/2, 914c clear events=7/start=8. 9150 bit2 is completion; DMA leaves counter at 7.");
  setEOLComment(toAddr(0x20279c8),"Analog path 886c/8870/8874, clock source 8848 bits8:9 cleared, 884c bits24:27=b. DAC and shared clock gates 30/24 active low.");
  setEOLComment(toAddr(0x205e1c8),"Stock rates: 48000,44100,32000,0,24000,22050,16000,0,12000,11025,8000. Under SDK inherited clock profile selectors8/4/0 measure 8/16/32 kHz, approximately 2/3 of stock labels.");
  println("Audio labels/comments applied without creating functions.");
 }
}
