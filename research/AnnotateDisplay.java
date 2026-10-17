import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.CodeUnit;

/** Vendor image labels from raw JT5295 instructions and live SDK evidence.
 * Does not change language, instruction decoding, function bodies or types. */
public class AnnotateDisplay extends GhidraScript {
    public void run() throws Exception {
        long[] addresses={0x0202bcb0,0x0202bd14,0x0202bd68,0x0202bdec,
            0x0202be70,0x0202bf04,0x0202bf20,0x0202bfb8,0x0202c05c,
            0x0202c118,0x0202c15c,0x0202c190,0x0202c1d8,0x0202c23c,
            0x0202c284,0x0202c2bc,0x0202c4dc,0x0202f640,0x020305c0,
            0x020030cc,0x02002ca4,0x02002f78,0x02043660,0x020437b4};
        String[] names={"lcd_command_byte","lcd_data_byte","lcd_command_word","lcd_data_word",
            "lcd_bus_clock_init","lcd_bus_pulse_length","lcd_bus_signals","lcd_bus_mask",
            "lcd_bus_clock_divider","lcd_output_packing_words","lcd_frame_bytes_minus_one",
            "lcd_mcu_mode","lcd_controller_enable_irq","lcd_frame_mode","lcd_frame_trigger",
            "lcd_interrupt_handler","lcd_mcu_pinmux","lcd_command_table_execute","lcd_configure",
            "board_lcd_reset","board_lcd_open_backlight_off","board_lcd_backlight_control",
            "lcd_config_load_spi","board_lcd_initialize"};
        for(int i=0;i<addresses.length;++i) {
            createLabel(toAddr(addresses[i]),"jt5295_"+names[i],false);
            currentProgram.getListing().setComment(toAddr(addresses[i]),CodeUnit.PRE_COMMENT,
                "JT5295 SDK display trace: "+names[i]+". See appotech-sdk/research/display.md. " +
                "Verified with native little-endian raw instructions, PC+4 call links and no delay slots. " +
                "Generic OpenRISC decompilation may move arguments across calls incorrectly.");
            println(String.format("%08x %s",addresses[i],names[i]));
        }
        println("Display config: SPI file 0x96000, iLi9225G at 0x96038; command table 0x96284..0x96703.");
        println("Language retained: "+currentProgram.getLanguageID());
    }
}
