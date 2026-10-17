import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.SourceType;
public class AnnotateCamera extends GhidraScript {
 public void run() throws Exception {
  long[] a={0x02003144L,0x020031d8L,0x02026f7cL,0x02027080L,0x0202709cL,0x02027128L,0x02027178L,0x0202aa8cL,0x0202ad58L,0x0202adc8L,0x0202ae88L,0x0202af0cL,0x0202b2acL,0x0202b3acL,0x0202b994L,0x0202f02cL,0x0202f0ecL,0x02035ed8L,0x0203618cL,0x02036320L,0x0204295cL,0x020429d8L,0x02042a68L,0x02042f14L};
  String[] n={"Camera_board_reset_PD0_PD1","Camera_device_open","Camera_I2C_400k_initialize","Camera_I2C_start","Camera_I2C_stop","Camera_I2C_send_byte","Camera_I2C_receive_NACK","Camera_IRQ_dispatch","Camera_DMA_NV21_b050_b054","Camera_DMA_alternate_b040_b044","Camera_DVP_pinmux","Camera_MCLK_24MHz","Camera_input_dimensions","Camera_scaled_planes_b060_b064","Camera_capture_enable","Camera_sensor_write_8bit","Camera_sensor_read_8bit","Camera_sensor_pipeline_config","Camera_frame_allocate_NV21","Camera_service_initialize","Camera_resource_write_pair","Camera_resource_read_ID","Camera_detect_resource","Camera_sensor_clock_detect_setup"};
  for(int i=0;i<a.length;++i){createLabel(toAddr(a[i]),n[i],true,SourceType.USER_DEFINED);println(Long.toHexString(a[i])+" "+n[i]);}
  setPlateComment(toAddr(0x0202aa8cL),"Native JT5295 trace: b00c is W1C status. Live independent capture verified bit3 is frame START; bit0 is complete frame. Stopping at bit3 returns stale DDR. b018 bit19 enables b050/b054 native NV21 receive DMA. Polling SDK masks PIC14; no vendor callback execution. See research/camera.md.");
  setPlateComment(toAddr(0x02042a68L),"Selected resource SPI offset96704: one 72-byte entry, expected ID39 at page0/reg1, I2C addresses42/43 (7bit21), 96-byte config at9678c, 349 register/value pairs + ffff sentinel at967ec. Live stock RAM and independent I2C confirmed this exact selection; native sensor image640x480.");
  println("Camera labels/comments applied; raw native instructions authoritative over generic OpenRISC delay-slot decompilation.");
 }
}
