#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define assert(expression) \
    do { if(!(expression)) { \
        console_printf("Assertion failed: " _PDCLIB_symbol2string(expression)\
                         ", function ", __func__,                         \
                         ", file " __FILE__ \
                         ", line " _PDCLIB_symbol2string( __LINE__ ) \
                         "." _PDCLIB_endl ); \
        while(1);                          \
      } \
    } while(0)

#include "serial.h"
#include "console.h"
#include "atags.h"
#include "armpte.h"
#include <kevlar/memregions.h>

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))

#define ARM_SCTLR_M     0x1 /* MMU enable */
#define ARM_SCTLR_V     0x2000 /* vectors base (high vs VBAR) */
#define ARM_SCTLR_VE    0x1000000 /* interrupt vectors enable */
#define ARM_SCTLR_AFE   (1UL << 29) /* access flag enable -- simplified PTEs */

#define ARM_SCR_NS      0x01 // non-secure bit

// defined in kevlar linker script
extern char monitor_image_start, monitor_image_data, monitor_image_end, _monitor_start;

void park_secondary_cores(void);

static inline uint8_t mycoreid(void)
{
    uint32_t val;
    __asm("mrc p15, 0, %0, c0, c0, 5" : "=r" (val));
    return val & 0xff;
}

extern void print_hex(uint32_t val);

static void secure_world_init(uintptr_t ptbase, uintptr_t vbar)
{
    uint32_t reg;
    /* setup secure-world page tables */

    /* load the same page table base into both TTBR0 and TTBR1
     * TTBR0 will change in the monitor's context switching code */
    assert((ptbase & 0x3fff) == 0);
    uintptr_t ttbr = ptbase | 0x6a; // XXX: cache pt walks, seems a good idea!
    __asm volatile("mcr p15, 0, %0, c2, c0, 0" :: "r" (ttbr));
    __asm volatile("mcr p15, 0, %0, c2, c0, 1" :: "r" (ttbr));

    /* setup TTBCR for a 2G/2G address split, and enable both TTBR0 and TTBR1 */
    __asm volatile("mcr p15, 0, %0, c2, c0, 2" :: "r" (7));

    /* set domain 0 to manager access (??) */
    __asm volatile("mcr p15, 0, %0, c3, c0, 0" :: "r" (3));

    /* flush stuff */
    __asm volatile("dsb");
    __asm volatile("isb");
    __asm volatile("mcr p15, 0, r0, c8, c7, 0"); // TLBIALL

    /* enable the MMU in the system control register
     * (this should be ok, since we have a 1:1 map for low RAM) */
    __asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r" (reg));
    reg |= ARM_SCTLR_M | ARM_SCTLR_AFE;
    // while we're here, ensure that there's no funny business with the VBAR
    reg &= ~(ARM_SCTLR_V | ARM_SCTLR_VE);
    console_printf("updating SCR to %lx\n", reg);
    __asm volatile("mcr p15, 0, %0, c1, c0, 0" : : "r" (reg));

    /* setup secure VBAR and MVBAR */
    __asm volatile("mcr p15, 0, %0, c12, c0, 0" :: "r" (vbar));
    __asm volatile("mcr p15, 0, %0, c12, c0, 1" :: "r" (vbar));

    /* flush again */
    __asm volatile("isb");
}

static volatile bool global_barrier;
static uintptr_t g_ptbase, g_mvbar;

static void __attribute__((noreturn)) secondary_main(uint8_t coreid)
{
    while (!global_barrier) __asm volatile("yield");

    secure_world_init(g_ptbase, g_mvbar);

    uintptr_t reg;
    __asm volatile("mrc p15, 0, %0, c1, c1, 0" : "=r" (reg));
    reg |= ARM_SCR_NS;
    __asm volatile("mcr p15, 0, %0, c1, c1, 0" : : "r" (reg));
    __asm volatile("isb");

    park_secondary_cores();
    
    /* TODO */
    while (1) {}
}

static void direct_map_section(armpte_short_l1 *l1pt, uintptr_t addr)
{
    uintptr_t idx = addr >> 20;

    l1pt[idx].raw = (armpte_short_l1) {
        .section = {
            .type = 1,
            .b = 1, // write-back, write-allocate
            .c = 0, // write-back, write-allocate
            .xn = 0,
            .domain = 0,
            .ap0 = 1, // access flag = 1 (already accessed)
            .ap1 = 0, // system
            .tex = 5, // 0b101: cacheable, write-back, write-allocate
            .ap2 = 0,
            .s = 1, // shareable
            .ng = 0, // global (ASID doesn't apply)
            .ns = 0, // secure-world PA, not that it makes a difference on Pi
            .secbase = idx,
        }
    }.raw;
}

static void map_l2_pages(armpte_short_l2 *l2pt, uintptr_t vaddr, uintptr_t paddr,
                         size_t bytes, bool exec)
{
    for (uintptr_t idx = (vaddr >> 12) & 0xff; bytes > 0; idx++) {
        console_printf("map PA %lx at index %lx\n", paddr, idx);
        l2pt[idx].raw = (armpte_short_l2) {
            .smallpage = {
                .xn = exec ? 0 : 1,
                .type = 1,
                .b = 1, // write-back, write-allocate
                .c = 0, // write-back, write-allocate
                .ap0 = 1, // access flag = 1 (already accessed)
                .ap1 = 0, // system
                .tex = 5, // 0b101: cacheable, write-back, write-allocate
                .ap2 = exec ? 1 : 0,
                .s = 1, // shareable
                .ng = 0, // global (ASID doesn't apply)
                .base = paddr >> 12
            }
        }.raw;
        bytes -= 0x1000;
        paddr += 0x1000;
    }
}

static uintptr_t smc(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2)
{
    register uintptr_t r0 __asm("r0") = arg0;
    register uintptr_t r1 __asm("r1") = arg1;
    register uintptr_t r2 __asm("r2") = arg2;

    __asm("smc #0"
          : "+r" (r0), "+r" (r1), "+r" (r2)
          : "0" (r0), "1" (r1), "2" (r2)
          : "r3"
          );

    return r0;
}

static void smc_test(void)
{
    console_printf("SMC test...\n");

    uintptr_t ret = smc(0, 0, 0);

    console_printf("SMC returned: %lx\n", ret);
}

void __attribute__((noreturn)) main(void)
{
    uint8_t coreid = mycoreid();
    if (coreid != 0) {
        secondary_main(coreid);
    }

    serial_init();
    serial_putc('H');
    console_puts("ello world\n");

    uintptr_t reg;
    __asm("mrs %0, cpsr" : "=r" (reg));
    console_printf("Initial CPSR: %lx\n", reg);
    __asm("mrc p15, 0, %0, c1, c1, 0" : "=r" (reg));
    console_printf("Initial SCR: %lx\n", reg);

    /* dump ATAGS, and reserve some high RAM for monitor etc. */
    atags_init((void *)0x100);
    atags_dump();

    uintptr_t monitor_physbase, ptbase;
    monitor_physbase = atags_reserve_physmem(KEVLAR_MON_PHYS_RESERVE);

    /* copy the monitor image into place */
    console_printf("Copying monitor to %lx\n", monitor_physbase);
    size_t monitor_image_bytes = &monitor_image_end - &monitor_image_start;
    memcpy((void *)monitor_physbase, &monitor_image_start, monitor_image_bytes);

    console_puts("Constructing page tables\n");

    /* L1 page table must be 16kB-aligned */
    ptbase = monitor_physbase + ROUND_UP(monitor_image_bytes, 16 * 1024);

    armpte_short_l1 *l1pt = (void *)ptbase;
    armpte_short_l2 *l2pt = (void *)(ptbase + 16 * 1024);

    console_printf("L1 %p L2 %p\n", l1pt, l2pt);

    /* direct-map first 1MB of RAM and UART registers using section mappings */
    direct_map_section(l1pt, 0);
    direct_map_section(l1pt, 0x3f200000);

    /* install a second-level page table for the monitor image */
    l1pt[KEVLAR_MON_VBASE >> 20].raw = (armpte_short_l1){
        .pagetable = {
            .type = 1,
            .pxn = 0,
            .ns = 0, // secure world PA, not that it matters on Pi?
            .ptbase = ((uintptr_t)l2pt) >> 10,
        }
    }.raw;

    // text and rodata
    size_t monitor_executable_size = &monitor_image_data - &monitor_image_start;
    console_printf("mapping monitor executable at %lx-%lx\n",
                   KEVLAR_MON_VBASE, KEVLAR_MON_VBASE + monitor_executable_size);
    map_l2_pages(l2pt, KEVLAR_MON_VBASE, monitor_physbase,
                 monitor_executable_size, true);

    // data and bss
    console_printf("mapping monitor RW at %lx-%lx\n",
                   KEVLAR_MON_VBASE + monitor_executable_size,
                   KEVLAR_MON_VBASE + monitor_image_bytes);
    map_l2_pages(l2pt, KEVLAR_MON_VBASE + monitor_executable_size,
                 monitor_physbase + monitor_executable_size,
                 monitor_image_bytes - monitor_executable_size, false);

    uintptr_t monitor_entry
        = &_monitor_start - &monitor_image_start + KEVLAR_MON_VBASE;

    g_ptbase = ptbase;
    g_mvbar = KEVLAR_MON_VBASE;

    //print_hex(0x4237);
    //console_printf(" <-- Print_hex test\n"),

    secure_world_init(g_ptbase, g_mvbar);

    /* call into the monitor's init routine
     * this will return to us in non-secure world */
    console_printf("entering monitor at %lx\n", monitor_entry);
    typedef void entry_func(void);
    ((entry_func *)monitor_entry)();

    console_printf("returned from monitor!\n");

    global_barrier = true;

    __asm volatile("mrc p15, 0, %0, c1, c1, 0" : "=r" (reg));
    reg |= ARM_SCR_NS;
    __asm volatile("mcr p15, 0, %0, c1, c1, 0" : : "r" (reg));
    __asm volatile("isb");

    console_printf("exited secure world\n");

    smc_test();
    
    console_printf("entering kernel...\n");
    typedef void kernel_entry(uintptr_t zero, uintptr_t boardid, void *atags);
    ((kernel_entry *)0x8000)(0, 0xc43, (void *)0x100);

    while (1) {}
}

void data_abort_handler(void)
{
    uintptr_t dfar, dfsr;

    serial_putc('&');
    console_puts("\nData abort!\n");

    __asm("mrc p15, 0, %0, c5, c0, 0" : "=r" (dfsr));
    __asm("mrc p15, 0, %0, c6, c0, 0" : "=r" (dfar));

    console_printf ("DFAR %lx DFSR %lx\n", dfar, dfsr);

    while (1) {}
}
