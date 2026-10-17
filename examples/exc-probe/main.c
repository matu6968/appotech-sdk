/* Diagnostic for src/exception.c: waits 10 s, then provokes one event chosen by EXC_TEST (default 9).
 *   9  jump to the illegal-instruction vector slot (0x70): enters the handler exactly as the CPU would, and is the
 *      reliable way to exercise it. The camera prints one EXC line on UART0, resets, and the next boot reports
 *      the record in `debug info` ("previous_exception") and in the log.
 *   10 spin in a loop that never feeds the watchdog. Not reported: a hang that lets the watchdog expire resets the
 *      camera without any exception (unlike the software fast reset, which raises vector 0x30 and is therefore
 *      flagged by apx_exception_expect_reset() so it does not look like a crash).
 *   4  call through a NULL pointer (executes the vendor reset stub at SRAM 0; recorded as vector 0x30).
 * Found on hardware: l.sys, l.trap, reserved/custom opcodes and loads from unmapped addresses (2, 3, 5, 6, 7) never
 * reach the vector table; the camera just resets or hangs, so those cannot be reported by a handler. */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <stdio.h>
#ifndef EXC_TEST
#define EXC_TEST 9
#endif
int main(void) {
    apx_usb_init();
    printf("exc-probe: test %d in 10 s\n", EXC_TEST);
    uint32_t until = apx_ticks() + 32768u * 10u;
    while(!apx_deadline_expired(until)) apx_debug_poll();
    printf("exc-probe: provoking\n");
#if EXC_TEST == 2
    __asm__ volatile(".word 0xffffffff");
#elif EXC_TEST == 10
    for(;;) __asm__ volatile(""); /* a hang that lets the watchdog expire: it should report where it was stuck */
#elif EXC_TEST == 9
    { void (*volatile slot)(void) = (void (*)(void))(uintptr_t)0x70u; slot(); } /* enter the handler as if by an illegal-instruction exception */
#elif EXC_TEST == 8
    {   /* does l.sys reach slot 0xc0 and does l.rfe come back? */
        volatile uint32_t *slot = (volatile uint32_t *)0xc00u, *mark = (volatile uint32_t *)0x3500u;
        mark[0] = 0;
        slot[0] = 0x18600000u;          /* l.movhi r3,0x0   */
        slot[1] = 0xa8630000u | 0x3500; /* l.ori r3,r3,0x3500 */
        slot[2] = 0x9c800077u;          /* l.addi r4,r0,0x77 */
        slot[3] = 0xd4032000u;          /* l.sw 0(r3),r4 */
        slot[4] = 0x24000000u;          /* l.rfe */
        __asm__ volatile("l.sys 0");
        printf("exc-probe: l.sys returned, mark=%lx\n", (unsigned long)mark[0]);
    }
#elif EXC_TEST == 5
    __asm__ volatile("l.sys 0");
#elif EXC_TEST == 6
    __asm__ volatile("l.trap 0");
#elif EXC_TEST == 7
    __asm__ volatile(".word 0x7c000000"); /* reserved opcode */
#elif EXC_TEST == 3
    volatile uint32_t *bad = (volatile uint32_t *)(uintptr_t)0x7ffffff0u;
    printf("%lu\n", (unsigned long)*bad);
#else
    void (*volatile null)(void) = (void (*)(void))0;
    null();
#endif
    printf("exc-probe: no exception taken\n");
    for(;;) apx_debug_poll();
}
