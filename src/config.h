#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>

#define CONFIG_VGABIOS_PATH_MAX 256

enum csmwrap_cpu_filter_mode {
    CPU_FILTER_NONE = 0,
    CPU_FILTER_ALLOWLIST,
    CPU_FILTER_BLOCKLIST,
};

struct csmwrap_config {
    bool serial_debug;
    uint16_t serial_port;
    uint32_t serial_baud;
    CHAR16 vgabios_path[CONFIG_VGABIOS_PATH_MAX];
    bool iommu_disable;
    bool verbose;
    bool vga_specified;
    uint8_t vga_bus;
    uint8_t vga_device;
    uint8_t vga_function;

    bool system_thread_specified;
    uint32_t system_thread_apic_id;

    /*
     * cpu_filter_list points to an EFI-pool-allocated array of length
     * cpu_filter_count, or is NULL when cpu_filter_count == 0. An empty
     * list with mode ALLOWLIST means "no APs visible to the OS"; an
     * empty list with mode BLOCKLIST means "no APs hidden from the OS".
     */
    enum csmwrap_cpu_filter_mode cpu_filter_mode;
    uint32_t *cpu_filter_list;
    size_t cpu_filter_count;
};

extern struct csmwrap_config gConfig;

/*
 * Returns true if the CPU with the given APIC ID is allowed by the
 * user-configured allow/block list. Does NOT consider BSP-must-keep or
 * system-thread-must-hide rules - callers handle those separately.
 *
 * With CPU_FILTER_NONE this always returns true.
 */
bool config_cpu_in_filter(uint32_t apic_id);

/*
 * Load configuration from csmwrap.ini next to the running EFI executable.
 * root_dir: filesystem root opened via EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
 * file_path: loaded image's FilePath device path (used to find our directory)
 *
 * If the file is missing or unreadable, defaults are silently retained.
 */
void config_load(EFI_FILE_PROTOCOL *root_dir, EFI_DEVICE_PATH_PROTOCOL *file_path);

#endif
