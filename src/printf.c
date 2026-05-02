#include <stddef.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>

#define NANOPRINTF_IMPLEMENTATION
#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS 1
#include <nanoprintf.h>

#include <efi.h>
#include <csmwrap.h>
#include <config.h>
#include <io.h>
#include <flanterm.h>
#include <flanterm_backends/fb.h>

struct flanterm_context *flanterm_ctx = NULL;

static EFI_SERIAL_IO_PROTOCOL *serial_protocol = NULL;
static bool serial_protocol_init_done = false;
static bool direct_io_initialised = false;

static void serial_protocol_initialise(void) {
    if (serial_protocol_init_done) {
        return;
    }
    serial_protocol_init_done = true;

    EFI_GUID serial_guid = EFI_SERIAL_IO_PROTOCOL_GUID;
    if (gBS->LocateProtocol(&serial_guid, NULL, (void **)&serial_protocol)) {
        serial_protocol = NULL;
        return;
    }

    serial_protocol->Reset(serial_protocol);
    serial_protocol->SetAttributes(
        serial_protocol,
        gConfig.serial_baud,
        0,                /* ReceiveFifoDepth (default) */
        0,                /* Timeout (default) */
        NoParity,         /* Parity */
        8,                /* DataBits */
        OneStopBit        /* StopBits */
    );
}

static void serial_direct_io_initialise(void) {
    if (direct_io_initialised) {
        return;
    }

    uint16_t port = gConfig.serial_port;

    outb(port + 3, 0x00);
    outb(port + 1, 0x00);
    outb(port + 3, 0x80);

    uint16_t divisor = (uint16_t)(115200 / gConfig.serial_baud);
    if (divisor == 0) divisor = 1;
    outb(port + 0, divisor & 0xff);
    outb(port + 1, (divisor >> 8) & 0xff);

    /* port+1 is DLM while DLAB=1; clear DLAB before treating it as IER. */
    outb(port + 3, 0x03);
    outb(port + 2, 0xc7);
    outb(port + 4, 0x0b);

    direct_io_initialised = true;
}

static void serial_out(uint8_t b) {
    if (gBootServicesExited) {
        serial_direct_io_initialise();

        uint16_t port = gConfig.serial_port;
        while ((inb(port + 5) & 0x20) == 0);
        outb(port, b);
        return;
    }

    serial_protocol_initialise();
    if (serial_protocol == NULL) {
        return;
    }

    UINTN buf_size = 1;
    serial_protocol->Write(serial_protocol, &buf_size, &b);
}

static void _putchar(int character, void *extra_arg) {
    (void)extra_arg;

    if (character == '\n') {
        _putchar('\r', NULL);
    }

    if (gConfig.serial_debug) {
        serial_out((uint8_t)character);
    }

    if (gConfig.verbose && flanterm_ctx != NULL) {
        flanterm_write(flanterm_ctx, (const char *)&character, 1);
    }
}

int printf(const char *restrict fmt, ...) {
    va_list l;
    va_start(l, fmt);
    int ret = npf_vpprintf(_putchar, NULL, fmt, l);
    va_end(l);
    return ret;
}

int vprintf(const char *restrict fmt, va_list l) {
    return npf_vpprintf(_putchar, NULL, fmt, l);
}

int snprintf(char *buffer, size_t bufsz, const char *restrict fmt, ...) {
    va_list l;
    va_start(l, fmt);
    int ret = npf_vsnprintf(buffer, bufsz, fmt, l);
    va_end(l);
    return ret;
}
