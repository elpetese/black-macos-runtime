/**
 * x86 port I/O for recompiled code.
 *
 * The lifter turns `in`/`out` into recomp_port_in/recomp_port_out. Nothing on
 * the host backs an I/O port, so each one a title depends on is modelled here;
 * everything else reads 0, keeps nothing, and is reported once.
 *
 * 0x80C0 -- MCPX GPIO. Bit 5 carries the TV encoder's field signal: the D3D
 * vblank ISR computes the field it is scanning as !(in(0x80C0) >> 5) & 1 and
 * D3DDevice_GetDisplayFieldStatus hands that to callers. A field that never
 * changes means a player that waits for "the other field" never starts: Black's
 * XMV intro video (sub_0023F45C) sets its clock only on a field other than 1.
 * The bit alternates with every delivered vblank, as it does on an interlaced
 * display; the other bits keep whatever the title last wrote.
 */

#include <stdint.h>
#include <stdio.h>

extern uint32_t xbox_VblankCount(void);

#define PORT_MCPX_GPIO      0x80C0u
#define GPIO_FIELD_BIT      0x20u

static uint8_t g_gpio_latch;

static void report_unmodelled(uint32_t port, uint32_t size, int is_out, uint32_t value)
{
    static uint32_t seen[32];
    static unsigned nseen;
    unsigned i;

    for (i = 0; i < nseen; i++)
        if (seen[i] == port)
            return;
    if (nseen < sizeof seen / sizeof seen[0])
        seen[nseen++] = port;
    if (is_out)
        fprintf(stderr, "  [PORT] out 0x%04X/%u = 0x%08X (not modelled)\n", port, size, value);
    else
        fprintf(stderr, "  [PORT] in 0x%04X/%u (not modelled, reads 0)\n", port, size);
    fflush(stderr);
}

uint32_t recomp_port_in(uint32_t port, uint32_t size)
{
    if (port == PORT_MCPX_GPIO && size == 1) {
        uint8_t v = (uint8_t)(g_gpio_latch & ~GPIO_FIELD_BIT);
        if (xbox_VblankCount() & 1u)
            v |= GPIO_FIELD_BIT;
        return v;
    }
    report_unmodelled(port, size, 0, 0);
    return 0;
}

void recomp_port_out(uint32_t port, uint32_t size, uint32_t value)
{
    if (port == PORT_MCPX_GPIO && size == 1) {
        g_gpio_latch = (uint8_t)value;
        return;
    }
    report_unmodelled(port, size, 1, value);
}
