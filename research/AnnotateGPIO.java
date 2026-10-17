import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.CodeUnit;
public class AnnotateGPIO extends GhidraScript {
 public void run() throws Exception {
  long[] addresses={0x02002b14,0x02002bd0,0x02002ef0,0x0200326c,0x02003494,
   0x02023ab8,0x02023b5c,0x02023c28,0x02023cb8,0x02023dc0,0x02023e44,
   0x02023ed4,0x02023ff4,0x02027b10,0x02027b78,0x02027ba8,0x02027bcc,
   0x0202ebc0,0x02090408};
  String[] names={"buttons_open","blue_led_open","blue_led_ioctl","buttons_sample",
   "buttons_ioctl","gpio_direction","gpio_pulls","gpio_path_8140","gpio_latch_write",
   "gpio_pin_read","gpio_path_8100","gpio_select","gpio_force_drive",
   "adc_channel_enable","adc_clock_divider","adc_start","adc_wait_read",
   "gpio_configure","button_adc_thresholds"};
  for(int i=0;i<addresses.length;++i) {
   createLabel(toAddr(addresses[i]),"jt5295_"+names[i],true);
   currentProgram.getListing().setComment(toAddr(addresses[i]),CodeUnit.PRE_COMMENT,
    "JT5295 GPIO evidence: "+names[i]+". Bank stride 4; latch/input 8000, input direction 8040, pulls 8080/80c0, force high/low 81c0/8200, GPIO select 8380. Board PB0 active-high Power, ADC3 ladder: shutter=0, Down=306, Up=484, Left=658, Right=817 (tolerance 29), PB5 active-high blue LED. Stock also configures PB1 input; analog pad mapping unverified. Hardware confirmed 2026-10-14. See appotech-sdk/research/gpio.md and gpio-vendor-raw.disasm. Raw native LE/no-delay instructions take precedence over generic OpenRISC decompilation.");
   println(String.format("%08x %s",addresses[i],names[i]));
  }
 }
}
