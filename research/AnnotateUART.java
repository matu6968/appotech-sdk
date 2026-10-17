import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.CodeUnit;
public class AnnotateUART extends GhidraScript {
 public void run() throws Exception {
  long[] addresses={0x02024684,0x020246d4,0x02024774,0x020247e8,0x02033a68};
  String[] names={"uart_receive_irq","uart_pin_routes","uart_baud_init","uart_byte_tx","board_uart_init_115200"};
  for(int i=0;i<addresses.length;i++){
   createLabel(toAddr(addresses[i]),"jt5295_"+names[i],true);
   currentProgram.getListing().setComment(toAddr(addresses[i]),CodeUnit.PRE_COMMENT,
    "Raw LE/no-delay UART evidence 2026-10-15. 9080 control=10; 9084=(144000000/baud)-1; 9088 byte data; 908c bit1 TX complete, write1 acknowledges TX, bit0 RX pending, write10 acknowledges RX. RX bit0/ack10 independently observed in BootROM10395c. Native 9084 readback is zero; SDK restores the board-known inherited divider1249 when unavailable. Stock pin selector2 is 8340[9:5] TX, selector3[4:0] RX. Route argument0 explicitly prepares PA7 and selects TX6/RX0; normal stock board init argument1 selects TX1/RX1. Those alternate route pads remain unidentified. SDK driver uses explicit initialization, polling queues and field-preserving shutdown; physical serial validation is tracked separately. See appotech-sdk/research/uart-vendor.disasm.");
   println(String.format("%08x %s",addresses[i],names[i]));
  }
 }
}
