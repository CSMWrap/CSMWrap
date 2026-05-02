#include <efi.h>
#include <csmwrap.h>
#include <io.h>
#include <time.h>

uint64_t tsc_freq;          /* TSC ticks per second */
static uint64_t tsc_boot;  /* TSC value at calibration time */

/*
 * Try to determine TSC frequency from CPUID leaves.
 * Returns frequency in Hz, or 0 if not available.
 */
static uint64_t tsc_freq_from_cpuid(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Check max CPUID leaf */
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
    uint32_t max_leaf = eax;

    /* CPUID leaf 0x15: TSC/Crystal ratio and crystal frequency */
    if (max_leaf >= 0x15) {
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x15), "c"(0));
        if (eax != 0 && ebx != 0 && ecx != 0) {
            return (uint64_t)ecx * ebx / eax;
        }
    }

    return 0;
}

/*
 * Calibrate TSC frequency.
 * Prefers CPUID-based detection; falls back to gBS->Stall() calibration.
 * Must be called before ExitBootServices.
 */
void calibrate_tsc(void) {
    tsc_freq = tsc_freq_from_cpuid();

    if (tsc_freq == 0) {
        uint64_t start = rdtsc();
        gBS->Stall(1000);  /* 1ms */
        uint64_t end = rdtsc();
        tsc_freq = (end - start) * 1000;
    }

    tsc_boot = rdtsc();
}

uint64_t get_nanoseconds_since_boot(void) {
    if (tsc_freq == 0)
        return 0;
    uint64_t elapsed = rdtsc() - tsc_boot;
    uint64_t usec = elapsed / tsc_freq * 1000000
                  + elapsed % tsc_freq * 1000000 / tsc_freq;
    return usec * 1000;
}

void stall(uint64_t us) {
    uint64_t next_stop = rdtsc() + (tsc_freq * us + 999999) / 1000000;
    while (rdtsc() < next_stop);
}
