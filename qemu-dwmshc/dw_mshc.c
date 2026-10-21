/*
 * Synopsys DesignWare Mobile Storage Host (DW-MSHC), a behavioural model for testing the read-only
 * PIO driver of the Exynos 7870 iOS port (shim/exynos/dwmmc.c, bsd/dev/sweet_emmc.c).
 *
 * Scope: register file, command engine (R1/R2/R3 responses, response timeout), PIO reads through
 * the data FIFO with RXDR/DTO, automatic CMD12 after multi-block reads, card detect. NOT modelled:
 * DMA/IDMAC, interrupts beyond a level line, writes, clock/phase tuning, voltage switching, the
 * Exynos SMU. It is derived from the same reading of the databook as the driver, so it can find
 * logic and integration bugs, not misunderstandings of the real silicon.
 *
 * Like a bootloader would, the model first brings the attached eMMC card to the transfer state
 * (CMD0, CMD1, CMD2, CMD3 with RCA 1, CMD7), on the first register access.
 *
 * This code is licensed under the GPL.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/sd/sd.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_DW_MSHC "dw-mshc"
OBJECT_DECLARE_SIMPLE_TYPE(DwMshcState, DW_MSHC)
#define TYPE_DW_MSHC_BUS "dw-mshc-bus"

#define FIFO_WORDS 64
#define REG_WORDS  (0x200 / 4)
#define DATA_PORT_NEW 0x200
#define DATA_PORT_OLD 0x100

/* register offsets */
#define R_CTRL     0x00
#define R_CLKENA   0x10
#define R_CTYPE    0x18
#define R_BYTCNT   0x20
#define R_INTMASK  0x24
#define R_CMDARG   0x28
#define R_CMD      0x2c
#define R_RESP0    0x30
#define R_MINTSTS  0x40
#define R_RINTSTS  0x44
#define R_STATUS   0x48
#define R_FIFOTH   0x4c
#define R_CDETECT  0x50
#define R_VERID    0x6c
#define R_HCON     0x70

/* CTRL */
#define CTRL_RESET       (1u << 0)
#define CTRL_FIFO_RESET  (1u << 1)
#define CTRL_DMA_RESET   (1u << 2)
#define CTRL_INT_ENABLE  (1u << 4)
/* CMD */
#define CMD_START        (1u << 31)
#define CMD_UP_CLK       (1u << 21)
#define CMD_SEND_AUTO_STOP (1u << 12)
#define CMD_WR           (1u << 10)
#define CMD_DATA_EXPECT  (1u << 9)
#define CMD_RESP_LONG    (1u << 7)
#define CMD_RESP_EXPECT  (1u << 6)
/* RINTSTS */
#define INT_CD    (1u << 0)
#define INT_DTO   (1u << 3)
#define INT_RXDR  (1u << 5)
#define INT_RTO   (1u << 8)
/* STATUS */
#define ST_FIFO_EMPTY   (1u << 2)
#define ST_DATA_BUSY    (1u << 9)

struct DwMshcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq;

    uint32_t regs[REG_WORDS];
    uint32_t rintsts;            /* sticky bits; RXDR is derived */
    uint32_t fifo[FIFO_WORDS];
    int fifo_head, fifo_n;
    uint32_t xfer_left;          /* bytes the card still has to deliver */
    bool xfer_active;
    bool multi;
    bool auto_stop;
    bool stop_sent;
    bool booted;
};

static size_t card_cmd(DwMshcState *s, uint8_t cmd, uint32_t arg, uint8_t *resp, size_t respsz)
{
    SDRequest req = { .cmd = cmd, .arg = arg, .crc = 0 };

    return sdbus_do_command(&s->sdbus, &req, resp, respsz);
}

/* What the bootloader leaves behind: an initialised card in the transfer state, RCA 1. */
static void dw_mshc_boot_init(DwMshcState *s)
{
    uint8_t resp[16];

    s->booted = true;
    if (!sdbus_get_inserted(&s->sdbus)) {
        return;
    }
    card_cmd(s, 0, 0, resp, sizeof(resp));
    for (int i = 0; i < 200; i++) {
        size_t n = card_cmd(s, 1, 0x40ff8080, resp, sizeof(resp));
        if (n == 4 && (ldl_be_p(resp) & 0x80000000u)) {
            break;
        }
    }
    card_cmd(s, 2, 0, resp, sizeof(resp));
    card_cmd(s, 3, 1u << 16, resp, sizeof(resp));
    card_cmd(s, 7, 1u << 16, resp, sizeof(resp));
    s->regs[R_CTYPE / 4] = 1;     /* 4-bit bus, like a booted host */
    s->regs[R_CLKENA / 4] = 1;
}

static uint32_t rx_watermark(DwMshcState *s)
{
    return (s->regs[R_FIFOTH / 4] >> 16) & 0xfff;
}

static void dw_mshc_update_irq(DwMshcState *s)
{
    bool level = (s->regs[R_CTRL / 4] & CTRL_INT_ENABLE) &&
                 (s->rintsts & s->regs[R_INTMASK / 4]);

    qemu_set_irq(s->irq, level);
}

static void stop_card_if_needed(DwMshcState *s)
{
    uint8_t resp[16];

    if (s->multi && !s->stop_sent) {
        s->stop_sent = true;
        card_cmd(s, 12, 0, resp, sizeof(resp));
    }
}

/* Move data from the card into the FIFO as far as there is room. */
static void dw_mshc_refill(DwMshcState *s)
{
    while (s->xfer_active && s->xfer_left && s->fifo_n < FIFO_WORDS && sdbus_data_ready(&s->sdbus)) {
        uint32_t w;

        sdbus_read_data(&s->sdbus, &w, sizeof(w));
        s->fifo[(s->fifo_head + s->fifo_n) % FIFO_WORDS] = le32_to_cpu(w);
        s->fifo_n++;
        s->xfer_left -= 4;
    }
    if (s->xfer_active && s->xfer_left == 0) {
        s->rintsts |= INT_DTO;       /* everything is in the FIFO */
        if (s->auto_stop) {
            stop_card_if_needed(s);
        }
    }
}

static void dw_mshc_cancel(DwMshcState *s)
{
    if (s->xfer_active && s->xfer_left) {
        stop_card_if_needed(s);
    }
    s->xfer_active = false;
    s->xfer_left = 0;
    s->fifo_n = 0;
    s->fifo_head = 0;
}

static void dw_mshc_exec(DwMshcState *s, uint32_t cmd)
{
    uint8_t resp[16];
    uint32_t idx = cmd & 0x3f;

    if (cmd & CMD_UP_CLK) {
        return;                      /* clock update: nothing to do, no command-done */
    }
    size_t n = card_cmd(s, idx, s->regs[R_CMDARG / 4], resp, sizeof(resp));
    s->rintsts |= INT_CD;
    if (cmd & CMD_RESP_EXPECT) {
        if (!n) {
            s->rintsts |= INT_RTO;   /* the card did not answer */
            return;
        }
        if (n >= 16) {
            for (int i = 0; i < 4; i++) {
                s->regs[R_RESP0 / 4 + 3 - i] = ldl_be_p(resp + 4 * i);
            }
        } else {
            s->regs[R_RESP0 / 4] = ldl_be_p(resp);
        }
    }
    if (cmd & CMD_DATA_EXPECT) {
        if (cmd & CMD_WR) {
            qemu_log_mask(LOG_UNIMP, "dw-mshc: writes are not modelled\n");
            return;
        }
        s->fifo_n = 0;
        s->fifo_head = 0;
        s->xfer_left = s->regs[R_BYTCNT / 4];
        s->xfer_active = true;
        s->multi = (idx == 18);
        s->auto_stop = !!(cmd & CMD_SEND_AUTO_STOP);
        s->stop_sent = false;
        dw_mshc_refill(s);
    }
}

static uint32_t dw_mshc_status(DwMshcState *s)
{
    uint32_t st = (uint32_t)s->fifo_n << 17;

    if (!s->fifo_n) {
        st |= ST_FIFO_EMPTY;
    }
    if (s->xfer_active && (s->xfer_left || s->fifo_n)) {
        st |= ST_DATA_BUSY;
    }
    return st;
}

static uint32_t dw_mshc_rintsts(DwMshcState *s)
{
    uint32_t v = s->rintsts;

    if (s->xfer_active && (s->fifo_n > (int)rx_watermark(s) ||
                           (s->xfer_left == 0 && s->fifo_n > 0))) {
        v |= INT_RXDR;
    }
    return v;
}

static uint64_t dw_mshc_read(void *opaque, hwaddr addr, unsigned size)
{
    DwMshcState *s = opaque;
    uint32_t v = 0;

    if (!s->booted) {
        dw_mshc_boot_init(s);
    }
    if (addr == DATA_PORT_NEW || addr == DATA_PORT_OLD) {
        if (s->fifo_n) {
            v = s->fifo[s->fifo_head];
            s->fifo_head = (s->fifo_head + 1) % FIFO_WORDS;
            s->fifo_n--;
            dw_mshc_refill(s);
        }
        return v;
    }
    switch (addr) {
    case R_RINTSTS:
        return dw_mshc_rintsts(s);
    case R_MINTSTS:
        return dw_mshc_rintsts(s) & s->regs[R_INTMASK / 4];
    case R_STATUS:
        return dw_mshc_status(s);
    case R_CDETECT:
        return sdbus_get_inserted(&s->sdbus) ? 0 : 1;   /* active low: 0 = card present */
    case R_VERID:
        return 0x5342270a;
    case R_HCON:
        return 0x07cbd6c1;
    case R_CMD:
        return s->regs[R_CMD / 4] & ~CMD_START;
    default:
        if (addr < REG_WORDS * 4) {
            return s->regs[addr / 4];
        }
    }
    return 0;
}

static void dw_mshc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    DwMshcState *s = opaque;
    uint32_t v = (uint32_t)val;

    if (!s->booted) {
        dw_mshc_boot_init(s);
    }
    switch (addr) {
    case R_RINTSTS:
        s->rintsts &= ~v;            /* write 1 to clear */
        break;
    case R_CTRL:
        if (v & (CTRL_FIFO_RESET | CTRL_RESET)) {
            dw_mshc_cancel(s);
        }
        s->regs[R_CTRL / 4] = v & ~(CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
        break;
    case R_CMD:
        s->regs[R_CMD / 4] = v & ~CMD_START;
        if (v & CMD_START) {
            dw_mshc_exec(s, v);
        }
        break;
    default:
        if (addr < REG_WORDS * 4 && addr != DATA_PORT_NEW && addr != DATA_PORT_OLD) {
            s->regs[addr / 4] = v;
        }
    }
    dw_mshc_update_irq(s);
}

static const MemoryRegionOps dw_mshc_ops = {
    .read = dw_mshc_read,
    .write = dw_mshc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void dw_mshc_reset(DeviceState *dev)
{
    DwMshcState *s = DW_MSHC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_FIFOTH / 4] = (31u << 16) | 32u;
    s->rintsts = 0;
    s->fifo_n = s->fifo_head = 0;
    s->xfer_left = 0;
    s->xfer_active = s->multi = s->auto_stop = s->stop_sent = false;
    s->booted = false;
    dw_mshc_update_irq(s);
}

static void dw_mshc_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    DwMshcState *s = DW_MSHC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &dw_mshc_ops, s, "dw-mshc", 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_DW_MSHC_BUS, dev, "sd-bus");
}

static void dw_mshc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *k = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(k, dw_mshc_reset);
    k->user_creatable = false;
}

static const TypeInfo dw_mshc_info[] = {
    {
        .name           = TYPE_DW_MSHC,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(DwMshcState),
        .instance_init  = dw_mshc_init,
        .class_init     = dw_mshc_class_init,
    },
    {
        .name           = TYPE_DW_MSHC_BUS,
        .parent         = TYPE_SD_BUS,
        .instance_size  = sizeof(SDBus),
    },
};

DEFINE_TYPES(dw_mshc_info)
