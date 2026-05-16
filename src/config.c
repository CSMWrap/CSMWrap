#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <efi.h>
#include <csmwrap.h>
#include <config.h>
#include <printf.h>

struct csmwrap_config gConfig = {
    .serial_debug = false,
    .serial_port = 0x3f8,
    .serial_baud = 115200,
    .vgabios_path = {0},
    .iommu_disable = true,
    .verbose = false,
    .vga_specified = false,
    .vga_bus = 0,
    .vga_device = 0,
    .vga_function = 0,
    .system_thread_specified = false,
    .system_thread_apic_id = 0,
    .cpu_filter_mode = CPU_FILTER_NONE,
    .cpu_filter_list = NULL,
    .cpu_filter_count = 0,
};

bool config_cpu_in_filter(uint32_t apic_id)
{
    if (gConfig.cpu_filter_mode == CPU_FILTER_NONE)
        return true;

    bool found = false;
    for (size_t i = 0; i < gConfig.cpu_filter_count; i++) {
        if (gConfig.cpu_filter_list[i] == apic_id) {
            found = true;
            break;
        }
    }

    if (gConfig.cpu_filter_mode == CPU_FILTER_ALLOWLIST)
        return found;
    /* CPU_FILTER_BLOCKLIST */
    return !found;
}

static bool char_eq_nocase(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    return a == b;
}

static bool streq_nocase(const char *a, const char *b)
{
    while (*a && *b) {
        if (!char_eq_nocase(*a, *b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool parse_bool(const char *val, bool *out)
{
    if (streq_nocase(val, "true") || streq_nocase(val, "yes") || streq_nocase(val, "1")) {
        *out = true;
        return true;
    }
    if (streq_nocase(val, "false") || streq_nocase(val, "no") || streq_nocase(val, "0")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_uint32(const char *val, uint32_t *out)
{
    uint32_t result = 0;
    bool hex = false;

    if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
        hex = true;
        val += 2;
    }

    if (*val == '\0')
        return false;

    while (*val) {
        char c = *val;
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (hex && c >= 'a' && c <= 'f') {
            digit = 10 + c - 'a';
        } else if (hex && c >= 'A' && c <= 'F') {
            digit = 10 + c - 'A';
        } else {
            return false;
        }
        result = result * (hex ? 16 : 10) + digit;
        val++;
    }

    *out = result;
    return true;
}

static bool parse_hex_byte(const char *s, size_t len, uint32_t *out)
{
    if (len == 0)
        return false;

    uint32_t result = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        uint32_t digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F')
            digit = 10 + c - 'A';
        else
            return false;
        result = result * 16 + digit;
    }
    *out = result;
    return true;
}

/*
 * Parse a single token (already null-terminated) of the form "N" or "N-M"
 * into either a single value or an inclusive range. Both N and M are
 * decimal or 0x-prefixed hex. Whitespace around the '-' is allowed.
 *
 * Returns false on malformed input or if N > M.
 */
static bool parse_apic_id_token(char *tok, uint32_t *lo, uint32_t *hi)
{
    char *dash = NULL;
    /* Skip a leading 0x prefix when scanning for '-' so we don't mistake
     * the hex digit sequence for a range delimiter (no negatives allowed). */
    char *scan = tok;
    if (scan[0] == '0' && (scan[1] == 'x' || scan[1] == 'X'))
        scan += 2;
    for (char *q = scan; *q; q++) {
        if (*q == '-') { dash = q; break; }
    }

    if (!dash) {
        uint32_t v;
        if (!parse_uint32(tok, &v))
            return false;
        *lo = *hi = v;
        return true;
    }

    /* Split into "lo" and "hi" halves, trimming whitespace around the dash. */
    char *lo_end = dash;
    while (lo_end > tok && (lo_end[-1] == ' ' || lo_end[-1] == '\t'))
        lo_end--;
    char *hi_start = dash + 1;
    while (*hi_start == ' ' || *hi_start == '\t')
        hi_start++;

    if (lo_end == tok || *hi_start == '\0')
        return false;

    *lo_end = '\0';

    uint32_t lo_v, hi_v;
    if (!parse_uint32(tok, &lo_v) || !parse_uint32(hi_start, &hi_v))
        return false;
    if (lo_v > hi_v)
        return false;

    *lo = lo_v;
    *hi = hi_v;
    return true;
}

/*
 * Parse a comma-separated list of APIC IDs.
 *
 * Each entry is either a single ID (decimal or 0x-prefixed hex) or an
 * inclusive range "N-M". Whitespace around commas and dashes is ignored.
 *
 * If out is non-NULL, expanded IDs are written to out[0 .. *out_count - 1]
 * and parsing fails if more than out_capacity entries would be produced.
 * If out is NULL, the call counts entries without writing anything; this
 * lets callers size an allocation up front.
 *
 * Returns false on malformed input.
 */
static bool parse_apic_id_list(const char *val, uint32_t *out,
                               size_t out_capacity, size_t *out_count)
{
    size_t count = 0;
    const char *p = val;

    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == ',') {
            if (*p == ',') p++;
            continue;
        }

        const char *start = p;
        while (*p && *p != ',')
            p++;

        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;

        char buf[64];
        size_t len = (size_t)(end - start);
        if (len == 0 || len >= sizeof(buf))
            return false;
        for (size_t i = 0; i < len; i++)
            buf[i] = start[i];
        buf[len] = '\0';

        uint32_t lo, hi;
        if (!parse_apic_id_token(buf, &lo, &hi))
            return false;

        for (uint32_t v = lo; ; v++) {
            if (out != NULL) {
                if (count >= out_capacity)
                    return false;
                out[count] = v;
            }
            /* Guard against size_t overflow: a single uint32_t range can
             * span 2^32 entries, which wraps size_t on 32-bit builds. */
            if (count == SIZE_MAX)
                return false;
            count++;
            if (v == hi)
                break;
        }

        if (*p == ',')
            p++;
    }

    *out_count = count;
    return true;
}

/*
 * Parse a PCI address in BB:DD.F format (all hex).
 */
static bool parse_pci_address(const char *val, uint8_t *bus, uint8_t *device, uint8_t *function)
{
    /* Find ':' separator between bus and device */
    const char *colon = NULL;
    for (const char *p = val; *p; p++) {
        if (*p == ':') { colon = p; break; }
    }
    if (!colon)
        return false;

    /* Find '.' separator between device and function */
    const char *dot = NULL;
    for (const char *p = colon + 1; *p; p++) {
        if (*p == '.') { dot = p; break; }
    }
    if (!dot)
        return false;

    uint32_t b, d, f;
    if (!parse_hex_byte(val, (size_t)(colon - val), &b) || b > 0xFF)
        return false;
    if (!parse_hex_byte(colon + 1, (size_t)(dot - colon - 1), &d) || d > 0x1F)
        return false;
    size_t flen = 0;
    while (dot[1 + flen]) flen++;
    if (!parse_hex_byte(dot + 1, flen, &f) || f > 0x7)
        return false;

    *bus = (uint8_t)b;
    *device = (uint8_t)d;
    *function = (uint8_t)f;
    return true;
}

static const char *skip_whitespace(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static size_t trim_trailing(const char *start, size_t len)
{
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' ||
                       start[len - 1] == '\r' || start[len - 1] == '\n'))
        len--;
    return len;
}

/*
 * Parse a single key=value line and apply it to gConfig.
 * key and val are null-terminated, trimmed strings.
 */
static void config_apply(const char *key, const char *val)
{
    if (streq_nocase(key, "serial")) {
        bool v;
        if (parse_bool(val, &v)) {
            gConfig.serial_debug = v;
            printf("  serial = %s\n", v ? "true" : "false");
        } else {
            printf("  warning: invalid value for 'serial': %s\n", val);
        }
    } else if (streq_nocase(key, "serial_port")) {
        uint32_t v;
        if (parse_uint32(val, &v) && v <= 0xFFFF) {
            gConfig.serial_port = (uint16_t)v;
            printf("  serial_port = 0x%x\n", gConfig.serial_port);
        } else {
            printf("  warning: invalid value for 'serial_port': %s\n", val);
        }
    } else if (streq_nocase(key, "serial_baud")) {
        uint32_t v;
        if (parse_uint32(val, &v) && v > 0) {
            gConfig.serial_baud = v;
            printf("  serial_baud = %u\n", gConfig.serial_baud);
        } else {
            printf("  warning: invalid value for 'serial_baud': %s\n", val);
        }
    } else if (streq_nocase(key, "vgabios")) {
        /* Convert ASCII path to CHAR16 */
        size_t i;
        for (i = 0; val[i] && i < CONFIG_VGABIOS_PATH_MAX - 1; i++)
            gConfig.vgabios_path[i] = (CHAR16)(unsigned char)val[i];
        gConfig.vgabios_path[i] = 0;
        printf("  vgabios = %s\n", val);
    } else if (streq_nocase(key, "iommu_disable")) {
        bool v;
        if (parse_bool(val, &v)) {
            gConfig.iommu_disable = v;
            printf("  iommu_disable = %s\n", v ? "true" : "false");
        } else {
            printf("  warning: invalid value for 'iommu_disable': %s\n", val);
        }
    } else if (streq_nocase(key, "verbose")) {
        bool v;
        if (parse_bool(val, &v)) {
            gConfig.verbose = v;
            printf("  verbose = %s\n", v ? "true" : "false");
        } else {
            printf("  warning: invalid value for 'verbose': %s\n", val);
        }
    } else if (streq_nocase(key, "vga")) {
        uint8_t b, d, f;
        if (parse_pci_address(val, &b, &d, &f)) {
            gConfig.vga_specified = true;
            gConfig.vga_bus = b;
            gConfig.vga_device = d;
            gConfig.vga_function = f;
            printf("  vga = %02x:%02x.%x\n", b, d, f);
        } else {
            printf("  warning: invalid PCI address for 'vga': %s (expected BB:DD.F)\n", val);
        }
    } else if (streq_nocase(key, "system_thread")) {
        uint32_t v;
        if (parse_uint32(val, &v)) {
            gConfig.system_thread_specified = true;
            gConfig.system_thread_apic_id = v;
            printf("  system_thread = APIC ID %u\n", v);
        } else {
            printf("  warning: invalid value for 'system_thread': %s\n", val);
        }
    } else if (streq_nocase(key, "cpu_allowlist") ||
               streq_nocase(key, "cpu_blocklist")) {
        enum csmwrap_cpu_filter_mode mode =
            streq_nocase(key, "cpu_allowlist") ? CPU_FILTER_ALLOWLIST
                                               : CPU_FILTER_BLOCKLIST;
        if (gConfig.cpu_filter_mode != CPU_FILTER_NONE &&
            gConfig.cpu_filter_mode != mode) {
            printf("  warning: '%s' ignored - cpu_allowlist and cpu_blocklist "
                   "are mutually exclusive\n", key);
        } else {
            /* First pass: validate syntax and compute the expanded count. */
            size_t count = 0;
            if (!parse_apic_id_list(val, NULL, 0, &count)) {
                printf("  warning: invalid value for '%s': %s\n", key, val);
            } else if (count > SIZE_MAX / sizeof(uint32_t)) {
                /* Allocation size would wrap size_t. */
                printf("  warning: '%s' has too many entries (%zu)\n",
                       key, count);
            } else {
                uint32_t *list = NULL;
                if (count > 0) {
                    EFI_STATUS st = gBS->AllocatePool(
                        EfiLoaderData, count * sizeof(uint32_t),
                        (void **)&list);
                    if (EFI_ERROR(st)) {
                        printf("  warning: out of memory for '%s' (%zu IDs)\n",
                               key, count);
                        goto cpu_filter_done;
                    }

                    /* Second pass: actually fill the buffer. */
                    size_t actual = 0;
                    if (!parse_apic_id_list(val, list, count, &actual)
                            || actual != count) {
                        gBS->FreePool(list);
                        printf("  warning: parse mismatch for '%s'\n", key);
                        goto cpu_filter_done;
                    }
                }

                /* Replace any previously-set list. */
                if (gConfig.cpu_filter_list) {
                    gBS->FreePool(gConfig.cpu_filter_list);
                    gConfig.cpu_filter_list = NULL;
                }
                gConfig.cpu_filter_mode = mode;
                gConfig.cpu_filter_list = list;
                gConfig.cpu_filter_count = count;

                if (count == 0) {
                    printf("  %s = (empty)%s\n", key,
                           mode == CPU_FILTER_ALLOWLIST
                               ? " - only the BSP will be visible to the OS"
                               : " - no CPUs hidden");
                } else {
                    printf("  %s = %zu APIC ID(s):", key, count);
                    /* Cap the printed list so a giant range doesn't spam. */
                    size_t shown = count < 16 ? count : 16;
                    for (size_t i = 0; i < shown; i++)
                        printf(" %u", list[i]);
                    if (shown < count)
                        printf(" ... (+%zu more)", count - shown);
                    printf("\n");
                }
            }
        cpu_filter_done: ;
        }
    } else {
        printf("  warning: unknown config key '%s'\n", key);
    }
}

/*
 * Parse an INI-style buffer (flat key=value, no sections).
 */
static void config_parse(char *buf, size_t len)
{
    char *line = buf;
    char *end = buf + len;

    while (line < end) {
        /* Find end of line */
        char *eol = line;
        while (eol < end && *eol != '\n')
            eol++;

        /* Null-terminate the line */
        if (eol < end)
            *eol = '\0';

        const char *p = skip_whitespace(line);

        /* Skip empty lines and comments */
        if (*p == '\0' || *p == ';' || *p == '#') {
            line = eol + 1;
            continue;
        }

        /* Find '=' separator */
        const char *eq = p;
        while (*eq && *eq != '=')
            eq++;

        if (*eq != '=') {
            printf("  warning: malformed line (no '='): %s\n", p);
            line = eol + 1;
            continue;
        }

        /* Extract key: from p to eq, trimmed */
        size_t key_len = trim_trailing(p, (size_t)(eq - p));
        if (key_len == 0) {
            line = eol + 1;
            continue;
        }

        /* Null-terminate key in place */
        char *key_start = (char *)p;
        key_start[key_len] = '\0';

        /* Extract value: after '=', trimmed */
        const char *val_start = skip_whitespace(eq + 1);
        size_t val_len = trim_trailing(val_start, eol - val_start);

        /* Null-terminate value in place */
        char *val_mut = (char *)val_start;
        val_mut[val_len] = '\0';

        config_apply(key_start, val_mut);

        line = eol + 1;
    }
}

/*
 * Build the config file path by finding the directory of the running
 * EFI executable from its device path and appending "csmwrap.ini".
 */
static bool config_build_path(EFI_DEVICE_PATH_PROTOCOL *file_path,
                              CHAR16 *out, size_t out_chars)
{
    if (!file_path)
        return false;

    /* Reconstruct the file path string from FILEPATH_DEVICE_PATH nodes */
    size_t pos = 0;
    out[0] = 0;

    EFI_DEVICE_PATH_PROTOCOL *node;
    for (node = file_path; !IsDevicePathEnd(node); node = NextDevicePathNode(node)) {
        if (DevicePathType(node) == MEDIA_DEVICE_PATH &&
            DevicePathSubType(node) == MEDIA_FILEPATH_DP) {
            FILEPATH_DEVICE_PATH *fp = (FILEPATH_DEVICE_PATH *)node;
            CHAR16 *src = fp->PathName;
            while (*src && pos < out_chars - 1) {
                out[pos++] = *src++;
            }
        }
    }
    out[pos] = 0;

    if (pos == 0)
        return false;

    /* Find the last backslash to strip the filename */
    size_t last_sep = 0;
    bool found_sep = false;
    for (size_t i = 0; i < pos; i++) {
        if (out[i] == L'\\' || out[i] == L'/') {
            last_sep = i;
            found_sep = true;
        }
    }

    size_t dir_end;
    if (found_sep) {
        dir_end = last_sep + 1; /* keep the trailing backslash */
    } else {
        /* No separator - file is at root, prepend backslash */
        dir_end = 0;
        if (pos + 1 < out_chars) {
            out[0] = L'\\';
            dir_end = 1;
        }
    }

    /* Append "csmwrap.ini" */
    static const CHAR16 ini_name[] = L"csmwrap.ini";
    size_t name_len = sizeof(ini_name) / sizeof(CHAR16) - 1;
    if (dir_end + name_len >= out_chars)
        return false;

    for (size_t i = 0; i <= name_len; i++)
        out[dir_end + i] = ini_name[i];

    return true;
}

/*
 * GUID for the CsmWrap EFI variable namespace. Randomly generated; it does
 * not derive from any vendor (e.g. Apple) namespace.
 * {2910aadb-f7ca-42b5-8ce2-d62d6a69b5ee}
 *
 * The variable holds raw config bytes, parsed exactly like csmwrap.ini:
 * newline-separated, one key=value per line. Authoring via the UEFI Shell
 * "setvar" command is intentionally not supported - its quoted-string form
 * cannot carry a literal newline, so multi-key config is impossible without
 * an in-firmware escape codec that collided with backslash paths.
 *
 * Write it from Linux via efivarfs instead, reusing the exact csmwrap.ini.
 * It must be one printf (efivarfs does a complete variable set per write()):
 * \x07\x00\x00\x00 is the NV|BS|RT attribute mask efivarfs expects, and the
 * double-quoted %s argument is emitted verbatim so the file's newlines and
 * backslash paths survive unmangled:
 *
 *   printf '\x07\x00\x00\x00%s' "$(cat csmwrap.ini)" > /sys/firmware/efi/efivars/CSMWrapConfig-2910aadb-f7ca-42b5-8ce2-d62d6a69b5ee
 */
#define CSMWRAP_VAR_NAME   L"CSMWrapConfig"
#define CSMWRAP_VAR_GUID   { 0x2910aadb, 0xf7ca, 0x42b5, \
                             { 0x8c, 0xe2, 0xd6, 0x2d, 0x6a, 0x69, 0xb5, 0xee } }

static bool config_load_from_nvram(void)
{
    EFI_GUID guid = CSMWRAP_VAR_GUID;
    UINTN data_size = 0;

    EFI_STATUS st = gRT->GetVariable(CSMWRAP_VAR_NAME, &guid,
                                     NULL, &data_size, NULL);
    if (st != EFI_BUFFER_TOO_SMALL) {
        return false;
    }

    printf("Config NVRAM: variable found, data_size=%lu\n", (unsigned long)data_size);

    if (data_size == 0 || data_size > 64 * 1024) {
        return false;
    }

    UINT8 *raw = NULL;
    if (gBS->AllocatePool(EfiLoaderData, data_size + 1, (void **)&raw) != EFI_SUCCESS) {
        return false;
    }
    raw[data_size] = 0;

    UINT32 attrs = 0;
    st = gRT->GetVariable(CSMWRAP_VAR_NAME, &guid, &attrs, &data_size, raw);
    if (EFI_ERROR(st)) {
        gBS->FreePool(raw);
        return false;
    }

    printf("Config NVRAM: read %lu bytes, attrs=0x%x\n",
           (unsigned long)data_size, (unsigned int)attrs);

    /*
     * The data is consumed verbatim, byte-for-byte, exactly like the
     * csmwrap.ini file: no encoding detection, no escape translation. Write
     * it as raw bytes with real newlines (see the efivarfs note above).
     */
    char *buf = (char *)raw;
    size_t buf_len = data_size;
    buf[buf_len] = '\0';

    printf("Config NVRAM: final config string (%lu chars):\n%s\n", (unsigned long)buf_len, buf);

    config_parse(buf, buf_len);

    gBS->FreePool(buf);
    return true;
}

/*
 * Try to load and parse the .ini file sitting next to the EFI binary.
 * Returns true only if a config file was found and parsed; any failure
 * (no filesystem, unbuildable path, missing/empty/oversized/unreadable
 * file) returns false so the caller can fall back to NVRAM.
 */
static bool config_load_from_file(EFI_FILE_PROTOCOL *root_dir,
                                  EFI_DEVICE_PATH_PROTOCOL *file_path)
{
    if (!root_dir || !file_path)
        return false;

    CHAR16 path[512];
    if (!config_build_path(file_path, path, ARRAY_SIZE(path))) {
        printf("Config: could not determine executable directory\n");
        return false;
    }

    EFI_FILE_PROTOCOL *file = NULL;
    if (root_dir->Open(root_dir, &file, path, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
        /* Not an error - config file is optional */
        return false;
    }

    /* Get file size via EFI_FILE_INFO */
    EFI_GUID fi_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 0;
    file->GetInfo(file, &fi_guid, &info_size, NULL);

    void *info_buf = NULL;
    if (gBS->AllocatePool(EfiLoaderData, info_size, &info_buf) != EFI_SUCCESS) {
        file->Close(file);
        return false;
    }

    UINTN file_size = 0;
    if (file->GetInfo(file, &fi_guid, &info_size, info_buf) == EFI_SUCCESS) {
        EFI_FILE_INFO *fi = info_buf;
        file_size = (UINTN)fi->FileSize;
    }
    gBS->FreePool(info_buf);

    if (file_size == 0 || file_size > 64 * 1024) {
        printf("Config: file empty or too large (%lu bytes)\n", (unsigned long)file_size);
        file->Close(file);
        return false;
    }

    /* Read file contents */
    char *buf = NULL;
    if (gBS->AllocatePool(EfiLoaderData, file_size + 1, (void **)&buf) != EFI_SUCCESS) {
        file->Close(file);
        return false;
    }

    UINTN read_size = file_size;
    if (file->Read(file, &read_size, buf) != EFI_SUCCESS) {
        gBS->FreePool(buf);
        file->Close(file);
        return false;
    }
    buf[read_size] = '\0';
    file->Close(file);

    printf("Config: loaded csmwrap.ini (%lu bytes)\n", (unsigned long)read_size);
    config_parse(buf, read_size);

    gBS->FreePool(buf);
    return true;
}

void config_load(EFI_FILE_PROTOCOL *root_dir, EFI_DEVICE_PATH_PROTOCOL *file_path)
{
    /* 1. Try the .ini file next to the EFI binary (original behaviour). */
    if (config_load_from_file(root_dir, file_path))
        return;

    /* 2. No usable .ini file - fall back to the NVRAM variable. This is the
     *    primary path when bundled as a coreboot EDK2 payload with no
     *    EFIESP partition available. */
    printf("Config: no .ini file found, trying NVRAM variable\n");
    config_load_from_nvram();
}
