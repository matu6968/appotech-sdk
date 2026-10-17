import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.pcode.PcodeOp;

public class VerifyJT5295 extends GhidraScript {
    public void run() throws Exception {
        if(!currentProgram.getLanguageID().toString().equals("OpenRISC:LE:32:JT5295"))
            throw new Exception("wrong language");
        new DisassembleCommand(toAddr(0x02180000),null,true).applyTo(currentProgram,monitor);
        int calls=0,branches=0;
        for(Instruction instruction:currentProgram.getListing().getInstructions(true)) {
            String mnemonic=instruction.getMnemonicString();
            if(instruction.getFlowType().isCall() || instruction.getFlowType().isJump()) {
                if(instruction.getDelaySlotDepth()!=0)throw new Exception("unexpected delay slot at "+instruction.getAddress());
                ++branches;
            }
            if(mnemonic.equals("l.jal") || mnemonic.equals("l.jalr")) {
                boolean link=false;
                for(PcodeOp op:instruction.getPcode())
                    if(op.getOpcode()==PcodeOp.COPY && op.getOutput()!=null &&
                       op.getOutput().getAddress().equals(currentProgram.getRegister("r9").getAddress()) &&
                       op.getInput(0).isConstant() &&
                       op.getInput(0).getOffset()==instruction.getAddress().getOffset()+4)link=true;
                if(!link)throw new Exception("wrong call link at "+instruction.getAddress());
                ++calls;
            }
        }
        if(calls==0)throw new Exception("no calls decoded");
        println("JT5295_LANGUAGE_PASS calls="+calls+" branches="+branches+" little_endian="+(!currentProgram.getLanguage().isBigEndian()));
    }
}
