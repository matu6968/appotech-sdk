import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.CodeUnit;
public class AnnotateBatterySPI extends GhidraScript {
 public void run() throws Exception {
  long[] addresses={0x02002d48,0x02003024,0x0202e758,0x0202e794,0x020267d0,0x02025750,0x02025970,0x020259c4,0x02025a18,0x02025ad0,0x02025b88,0x02032cfc,0x02032e8c,0x02032f54,0x02033070};
  String[] names={"battery_service","battery_boot_low_voltage_check","adc_channel_sample","battery_calibrated_voltage","external_supply_present","spi_controller_init","spi_byte_tx","spi_byte_rx","spi_dma_tx","spi_dma_rx","spi_chip_select","nor_jedec","nor_read","nor_page_program","nor_sector_erase"};
  for(int i=0;i<addresses.length;i++){
   createLabel(toAddr(addresses[i]),"jt5295_"+names[i],true);
   currentProgram.getListing().setComment(toAddr(addresses[i]),CodeUnit.PRE_COMMENT,
    "Verified raw little-endian JT5295/no-delay instructions 2026-10-15. Battery: 16 ADC9/11 pairs; millivolts=2*floor(mean9*SRAM[1fec]/mean11)+20; calibration readback803. Low SRAM804 is multiply/divide/double, not a BootROM API. Thresholds3660/3762/3860/3964/4060 mV; hysteresis30mV. 88e0bit21 stock supply-present; bit20 not identified as charging. SPI: pinsPA8/10/11,8344[21:20]=1,9000CSbit14,9244PIO1/DMA2,9248completion,9254wordcount-1,9258DMAaddress. SDK ports all sequences independently with bounded polling and cache maintenance. No vendor driver calls. See appotech-sdk/research/battery-spi.md; original SPI preserved, writes/erase model-tested only. Raw instructions take precedence over generic OpenRISC decompilation.");
   println(String.format("%08x %s",addresses[i],names[i]));
  }
 }
}
