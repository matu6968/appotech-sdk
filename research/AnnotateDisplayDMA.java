import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.symbol.Symbol;

/** Correct the early LCD pointer interpretation and annotate verified DMA. */
public class AnnotateDisplayDMA extends GhidraScript {
    public void run() throws Exception {
        for(Symbol s:currentProgram.getSymbolTable().getSymbols(toAddr(0x0202c118)))
            if(s.getName().equals("jt5295_lcd_framebuffer_addresses_formats"))s.delete();
        long[] addresses={0x0202c118,0x020229d8,0x02029cf4,0x02029d50,
            0x02029e18,0x02029f14,0x02029fb4,0x0202a070,0x0202a11c,
            0x0202a138,0x0202a844,0x02022c98};
        String[] names={"lcd_output_packing_words","cpu_irq_source_mask",
            "compositor_wait_frame","compositor_frame_start","compositor_output_geometry",
            "compositor_output_component_widths","compositor_yuv_conversion",
            "compositor_main_traversal","compositor_luma_chroma_addresses",
            "compositor_plane_pitch_words","compositor_scale_geometry_coefficients",
            "ddr_dma_range_maintenance"};
        for(int i=0;i<addresses.length;++i) {
            createLabel(toAddr(addresses[i]),"jt5295_"+names[i],true);
            currentProgram.getListing().setComment(toAddr(addresses[i]),CodeUnit.PRE_COMMENT,
                "JT5295 DMA trace: "+names[i]+". See appotech-sdk/research/display-dma.md. " +
                "Verified against native LE instructions and physical-camera full-frame NV21 DMA. " +
                "LCD a85c/a86c are packing configuration, NOT RAM pointers. " +
                "Source pointers are compositor c828/c830; YUV conversion and identity scaler are required. " +
                "The SDK requires 32-byte plane/pitch alignment. Generic OpenRISC delay-slot decompilation is unreliable here.");
            println(String.format("%08x %s",addresses[i],names[i]));
        }
    }
}
