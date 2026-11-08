/* Temporary ROM callback, linked at 0x3c20. No SPI access.
 * Host has backed up SRAM 0x3e8..0x1fff at 0x2000, and placed the verified
 * Silvergear bootstrap bytes at their native SRAM addresses. Invoke only its
 * PLL/DDR calibration entry (SPI 0x12fc -> SRAM 0x10fc), never its application.
 * Restore the overwritten SRAM before re-enabling the loader IRQs. */
    .nodelay
    .section .text.entry,"ax",@progbits
    .global silvergear_stage
silvergear_stage:
    l.sw -4(r1),r9
    l.sw -8(r1),r14
    l.sw -12(r1),r18
    l.addi r1,r1,-12
    l.ori r14,r3,0
    l.mfspr r18,r0,0x18
    l.addi r4,r0,-3
    l.and r4,r18,r4
    l.mtspr r0,r4,0x18
    l.ori r4,r0,0x5500
    l.mtspr r0,r4,0x880c
    l.addi r4,r0,1
    l.sw 4(r14),r4
    /* PLL calibration invalidates the live USB link. Detach before switching;
     * finish by entering the ROM USB initializer, never the old CDB callback. */
    l.ori r3,r0,0xa004
    l.mfspr r4,r3,0
    l.addi r5,r0,-65
    l.and r4,r4,r5
    l.mtspr r3,r4,0
    l.ori r4,r0,0x10fc
    l.jalr r4
    l.nop
    l.addi r3,r0,2
    l.sw 4(r14),r3
    l.ori r3,r0,0x1e14
    l.lwz r3,0(r3)
    l.sw 16(r14),r3
    /* Calibration is destructive. Clear DDR before the normal launch preflight. */
    l.movhi r3,0x0200
    l.movhi r4,0x0240
1:  l.sw 0(r3),r0
    l.addi r3,r3,4
    l.sfltu r3,r4
    l.bf 1b
    l.nop
    l.addi r3,r0,3
    l.sw 4(r14),r3
    l.ori r3,r0,0x3e8
    l.ori r4,r0,0x2000
    l.ori r5,r0,0x2000
2:  l.lwz r6,0(r4)
    l.sw 0(r3),r6
    l.addi r4,r4,4
    l.addi r3,r3,4
    l.sfltu r3,r5
    l.bf 2b
    l.nop
    l.lwz r3,16(r14)
    l.ori r4,r0,0x1e14
    l.sw 0(r4),r3
    /* Loader CPU clock pair; no USB controller, PLL2 or DDR-divider writes. */
    l.movhi r3,0x0b71
    l.ori r3,r3,0xb000
    l.ori r4,r0,0x1623
    .balignl 32,0x15000000
    l.mtspr r0,r3,0x885c
    l.mtspr r0,r4,0x8858
    l.addi r3,r0,500
3:  l.addi r3,r3,-1
    l.sfnei r3,0
    l.bf 3b
    l.nop
    l.ori r3,r0,0x721
    l.mtspr r0,r3,0x9800
    l.ori r3,r0,0x3fff
    l.mtspr r0,r3,0x9808
    l.mtspr r0,r0,0x9804
    l.sw 4(r14),r0
    l.mfspr r3,r0,0x8858
    l.sw 8(r14),r3
    l.mfspr r3,r0,0x885c
    l.sw 12(r14),r3
    l.addi r3,r0,-16391
    l.and r18,r18,r3
    l.mtspr r0,r18,0x18
    l.mfspr r3,r0,0x4800
    l.movhi r4,0xffff
    l.ori r4,r4,0x7fff
    l.and r3,r3,r4
    l.mtspr r0,r3,0x4800
    l.addi r1,r1,12
    l.lwz r14,-8(r1)
    l.lwz r18,-12(r1)
    l.movhi r9,0x0010
    l.ori r9,r9,0x385c
    l.jr r9
    l.nop
