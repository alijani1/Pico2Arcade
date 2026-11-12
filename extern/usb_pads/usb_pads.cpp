/* usb_pads.cpp - see usb_pads.h. Shared, unchanged, by Pico2Atari8 and
 * Pico2Arcade (moved out of Pico2Atari8's a8_input.cpp, 2026-11-11; the
 * comments below still name the Pico2Atari8 callers they were written for).
 * Pico2Atari8 compiles it inside a8_input.cpp (#include): an object file of
 * its own would reorder the other boards' linker veneers. Any other project
 * can compile it as a file of its own. */
#include <Arduino.h>
#include "Adafruit_TinyUSB.h"
#include "pio_usb.h"
#include "usb_pads.h"


static Adafruit_USBH_Host usb_host;
/* written by core 1 (the reports), read by core 0 */
/* (now both on the host's core: usb_pads_begin's) */
/* A field of a HID report: where it is (bits, after the report ID), its
 * size and range; size 0: none. */
struct HidField { uint16_t bit; uint8_t size; int32_t lmin, lmax; };
/* A pad's report, from its report descriptor (hid_parse_layout). */
struct PadLayout {
    bool ok;
    uint8_t id;                                          /* its report ID (0: none) */
    HidField x, y, hat;
    uint16_t btn_bit[16];                                /* button n + 1's bit; 0xFFFF: none */
};
struct UsbPad {
    bool on;
    uint8_t addr, idx;                                   /* its device, HID interface */
    uint16_t vid, pid;
    uint32_t buttons;                                    /* USBP_* held */
    uint16_t numbers;                                    /* buttons 1-16 held (bit n - 1) */
    uint32_t adirs;                                      /* USBP_* directions of its X / Y axes */
    int32_t ax, ay;                                      /* its X / Y as reported (lay.x / lay.y ranges) */
    uint8_t raw[16], raw_len;                            /* its last report as it came */
    uint32_t failing_since;                              /* its reports failing (0: they aren't) */
    bool fixed;                                          /* the SNES-style pad's known report */
    PadLayout lay;
};
static UsbPad usb_pads[2];                               /* player 1, player 2 (as they connect) */
static uint8_t usb_kbd[8];                               /* the last keyboard report */
static bool    usb_kbd_on = false;
static uint8_t usb_kbd_addr = 0;
static uint32_t usb_events = 0;                          /* mounts / unmounts: logged */

/* The SNES-style pad's report (and the many cheap pads that share it). */
static uint32_t usb_parse_pad(const uint8_t *r, uint16_t len) {
    uint32_t b = 0;
    if (len >= 2) {
        if (r[0] < 0x40) b |= USBP_LEFT;  else if (r[0] > 0xC0) b |= USBP_RIGHT;
        if (r[1] < 0x40) b |= USBP_UP;    else if (r[1] > 0xC0) b |= USBP_DOWN;
    }
    if (len >= 6) {
        static const uint32_t HAT[8] = { USBP_UP, USBP_UP | USBP_RIGHT, USBP_RIGHT, USBP_DOWN | USBP_RIGHT,
                                         USBP_DOWN, USBP_DOWN | USBP_LEFT, USBP_LEFT, USBP_UP | USBP_LEFT };
        uint8_t hat = r[5] & 0x0F;
        if (hat < 8) b |= HAT[hat];
        if (r[5] & 0x10) b |= USBP_X;
        if (r[5] & 0x20) b |= USBP_A;
        if (r[5] & 0x40) b |= USBP_B;
        if (r[5] & 0x80) b |= USBP_Y;
    }
    if (len >= 7) {
        if (r[6] & 0x01) b |= USBP_L;
        if (r[6] & 0x02) b |= USBP_R;
        if (r[6] & 0x10) b |= USBP_SELECT;
        if (r[6] & 0x20) b |= USBP_START;
    }
    return b;
}

/* The report descriptor's short items: the Generic Desktop page's X (0x30),
 * Y (0x31) and Hat switch (0x39), and the Button page's 1-16, in the first
 * report (ID) that has any of them; input fields only, and only inside a
 * Generic Desktop Joystick (0x04) or GamePad (0x05) application collection
 * - a mouse's X / Y and buttons (0e8f:3013's second interface has a mouse
 * and a keyboard collection) are not a pad. */
static void hid_parse_layout(const uint8_t *d, uint16_t len, PadLayout &L) {
    L = PadLayout();
    for (uint16_t &b : L.btn_bit) b = 0xFFFF;
    uint16_t page = 0, usages[16], umin = 0, umax = 0;
    uint8_t nus = 0, rsize = 0, rid = 0;
    uint16_t rcount = 0;
    int32_t lmin = 0, lmax = 0;
    bool urange = false;
    struct { uint8_t id; uint16_t bits; } offs[8];       /* each report's bits so far */
    uint8_t noffs = 0;
    int chosen = -1;
    int depth = 0, pad_depth = 0;                        /* collections; the pad one's depth */
    for (uint16_t i = 0; i < len;) {
        const uint8_t p = d[i++];
        if (p == 0xFE) {                                 /* a long item: skipped */
            if (i + 1 >= len) break;
            i += 2 + d[i];
            continue;
        }
        uint8_t sz = p & 3;
        if (sz == 3) sz = 4;
        const uint8_t type = (p >> 2) & 3, tag = p >> 4;
        if (i + sz > len) break;
        uint32_t u = 0;
        for (uint8_t k = 0; k < sz; k++) u |= (uint32_t)d[i + k] << (8 * k);
        int32_t s = (int32_t)u;
        if (sz == 1) s = (int8_t)u;
        else if (sz == 2) s = (int16_t)u;
        i += sz;
        if (type == 1) {                                 /* global */
            switch (tag) {
            case 0: page = (uint16_t)u; break;
            case 1: lmin = s; break;
            case 2: lmax = lmin >= 0 ? (int32_t)u : s; break;   /* (0..255 is often 1 byte) */
            case 7: rsize = (uint8_t)u; break;
            case 8: rid = (uint8_t)u; break;
            case 9: rcount = (uint16_t)u; break;
            default: break;
            }
        } else if (type == 2) {                          /* local */
            if (tag == 0 && nus < 16) usages[nus++] = (uint16_t)u;
            else if (tag == 1) { umin = (uint16_t)u; urange = true; }
            else if (tag == 2) umax = (uint16_t)u;
        } else if (type == 0) {                          /* main */
            if (tag == 0xA) {                            /* Collection */
                depth++;
                const uint16_t usage = nus ? usages[0] : 0;
                if (!pad_depth && u == 0x01 && page == 0x01 && (usage == 0x04 || usage == 0x05))
                    pad_depth = depth;                   /* an application: Joystick / GamePad */
            } else if (tag == 0xC) {                     /* End Collection */
                if (depth == pad_depth) pad_depth = 0;
                if (depth > 0) depth--;
            } else if (tag == 8) {                       /* Input (taken inside a pad's collection) */
                int o = -1;
                for (int k = 0; k < noffs; k++) if (offs[k].id == rid) o = k;
                if (o < 0 && noffs < 8) { o = noffs++; offs[o].id = rid; offs[o].bits = 0; }
                if (o < 0) break;
                const bool data_var = !(u & 1) && (u & 2);
                for (uint16_t f = 0; f < rcount; f++) {
                    const uint16_t bit = offs[o].bits;
                    offs[o].bits += rsize;
                    if (!data_var || !pad_depth || (chosen >= 0 && chosen != rid)) continue;
                    uint16_t usage = urange ? (uint16_t)(umin + f)
                                   : nus ? usages[f < nus ? f : nus - 1] : 0;
                    if (urange && usage > umax) continue;
                    const HidField fld = { bit, rsize, lmin, lmax };
                    bool used = false;
                    if (page == 0x01 && usage == 0x30 && !L.x.size) { L.x = fld; used = true; }
                    else if (page == 0x01 && usage == 0x31 && !L.y.size) { L.y = fld; used = true; }
                    else if (page == 0x01 && usage == 0x39 && !L.hat.size) { L.hat = fld; used = true; }
                    else if (page == 0x09 && usage >= 1 && usage <= 16 && L.btn_bit[usage - 1] == 0xFFFF) {
                        L.btn_bit[usage - 1] = bit;
                        used = true;
                    }
                    if (used) { chosen = rid; L.id = rid; L.ok = true; }
                }
            }
            nus = 0;                                     /* a main item ends the local ones */
            urange = false;
        }
    }
}

/* A field's value in a report (after its ID), or INT32_MIN if not in it. */
static int32_t hid_field(const uint8_t *r, uint16_t len, const HidField &f) {
    if (!f.size || f.size > 32 || f.bit + f.size > len * 8) return INT32_MIN;
    uint32_t v = 0;
    for (uint8_t b = 0; b < f.size; b++) {
        const uint16_t bit = f.bit + b;
        if ((r[bit >> 3] >> (bit & 7)) & 1) v |= 1u << b;
    }
    if (f.lmin < 0 && f.size < 32 && ((v >> (f.size - 1)) & 1)) v |= ~0u << f.size;   /* signed */
    return (int32_t)v;
}

/* Any pad, by its layout: false if the report isn't the one with the pad. */
static bool usb_parse_layout(const PadLayout &L, const uint8_t *r, uint16_t len, uint32_t *buttons,
                             uint16_t *numbers, uint32_t *adirs, int32_t *ax, int32_t *ay) {
    if (L.id) {
        if (!len || r[0] != L.id) return false;
        r++;
        len--;
    }
    uint32_t b = 0;
    /* the X / Y axes (an analog stick): their value as reported, and as
     * directions (past a quarter of the range) - apart from the buttons */
    const HidField *axes[2] = { &L.x, &L.y };
    int32_t *vals[2] = { ax, ay };
    uint32_t ad = 0;
    for (int a = 0; a < 2; a++) {
        const HidField &f = *axes[a];
        int32_t v = hid_field(r, len, f);
        if (v == INT32_MIN || f.lmax <= f.lmin) continue;
        *vals[a] = v;
        const int32_t c = f.lmin + (f.lmax - f.lmin) / 2, q = (f.lmax - f.lmin) / 4;
        if (v < c - q) ad |= a ? USBP_UP : USBP_LEFT;
        if (v > c + q) ad |= a ? USBP_DOWN : USBP_RIGHT;
    }
    *adirs = ad;
    const int32_t h = hid_field(r, len, L.hat);
    if (h != INT32_MIN) {
        static const uint32_t HAT[8] = { USBP_UP, USBP_UP | USBP_RIGHT, USBP_RIGHT, USBP_DOWN | USBP_RIGHT,
                                         USBP_DOWN, USBP_DOWN | USBP_LEFT, USBP_LEFT, USBP_UP | USBP_LEFT };
        const int32_t k = h - L.hat.lmin;                /* 8, 15 ...: centred */
        if (k >= 0 && k < 8) b |= HAT[k];
    }
    /* the buttons by number: 1 the main one (a joystick's trigger; the Quad
     * in controller mode sends Red as button 1) - B's slot, the first, the
     * main fire; then A, X, Y, L, R, L, R, Select, Start. (It was the cheap
     * pads' 1 X, 2 A, 3 B ...: the Quad's Red landed on X's slot, OPTION.) */
    static const uint32_t NUM[16] = { USBP_B, USBP_A, USBP_X, USBP_Y, USBP_L, USBP_R, USBP_L, USBP_R,
                                      USBP_SELECT, USBP_START, 0, 0, 0, 0, 0, 0 };
    uint16_t n = 0;
    for (int i = 0; i < 16; i++) {
        const uint16_t bit = L.btn_bit[i];
        if (bit == 0xFFFF || bit >= len * 8) continue;
        if ((r[bit >> 3] >> (bit & 7)) & 1) { n |= 1u << i; b |= NUM[i]; }
    }
    *buttons = b;
    *numbers = n;
    return true;
}

/* Every device that connects (a pad that isn't HID shows only here). */
extern "C" void tuh_mount_cb(uint8_t dev) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev, &vid, &pid);
    Serial.printf("[usb] device %u (%04x:%04x) connected%c", dev, vid, pid, 10);
}
extern "C" void tuh_umount_cb(uint8_t dev) { Serial.printf("[usb] device %u disconnected%c", dev, 10); }

/* (TinyUSB's header declares the HID mount / unmount callbacks weak, which
 * made these definitions weak too - and the linker took Adafruit's empty
 * ones: the pad was set up but never asked for a report. usb_pads_glue.c,
 * which doesn't include that header, defines them and calls these.) */
extern "C" void usb_pads_hid_mount(uint8_t dev, uint8_t idx, uint8_t const *desc, uint16_t desc_len) {
    uint8_t proto = tuh_hid_interface_protocol(dev, idx);
    if (proto == HID_ITF_PROTOCOL_KEYBOARD) {
        usb_kbd_addr = dev;
        usb_kbd_on = true;
    } else if (proto == HID_ITF_PROTOCOL_MOUSE) {
        return;                                          /* not used */
    } else {
        uint16_t vid = 0, pid = 0;
        tuh_vid_pid_get(dev, &vid, &pid);
        /* a pad only if its descriptor shows one (axes, hat or buttons), or
         * it is the SNES-style pad: a pad's other interfaces (0e8f:3013 has a
         * second one) were taken for a second pad with the SNES layout -
         * one pad took both players, and its garbage held the menus' UP */
        PadLayout lay{};                                 /* (zeroed: no descriptor - over the host's
                                                            256 bytes - is no layout, not stack junk) */
        if (desc && desc_len) hid_parse_layout(desc, desc_len, lay);
        const bool snes = vid == 0x081F && pid == 0xE401;
        /* a pad needs a left stick (X) or a D-pad (hat): an interface with
         * neither (buttons alone, or reports only) gets no player */
        if (!(lay.x.size || lay.hat.size) && !snes) {
            Serial.printf("[usb] %04x:%04x HID interface %u: not a pad (ignored)%c", vid, pid, idx, 10);
            return;
        }
        UsbPad *p = !usb_pads[0].on ? &usb_pads[0] : !usb_pads[1].on ? &usb_pads[1] : nullptr;
        if (!p) return;                                  /* a third: not used */
        *p = UsbPad();
        p->vid = vid;
        p->pid = pid;
        p->addr = dev;
        p->idx = idx;
        p->fixed = snes;                                 /* the SNES-style pad: as it was */
        p->lay = lay;
        int nb = 0;
        for (uint16_t bit : p->lay.btn_bit) nb += bit != 0xFFFF;
        Serial.printf("[usb] gamepad %d: %04x:%04x, %s: report %u, X %s, Y %s, hat %s, %d buttons%c",
                      (int)(p - usb_pads) + 1, vid, pid, p->fixed ? "SNES-style layout" : "its layout",
                      p->lay.id, p->lay.x.size ? "yes" : "no", p->lay.y.size ? "yes" : "no",
                      p->lay.hat.size ? "yes" : "no", nb, 10);
        p->on = true;
    }
    usb_events++;
    tuh_hid_receive_report(dev, idx);
}

extern "C" void usb_pads_hid_umount(uint8_t dev, uint8_t idx) {
    for (UsbPad &p : usb_pads)
        if (p.on && p.addr == dev && p.idx == idx) p = UsbPad();
    if (dev == usb_kbd_addr && usb_kbd_on) {
        usb_kbd_on = false;
        for (int i = 0; i < 8; i++) usb_kbd[i] = 0;
    }
    usb_events++;
}

extern "C" void tuh_hid_report_received_cb(uint8_t dev, uint8_t idx, uint8_t const *r, uint16_t len) {
    if (dev == usb_kbd_addr && usb_kbd_on && tuh_hid_interface_protocol(dev, idx) == HID_ITF_PROTOCOL_KEYBOARD) {
        for (int i = 0; i < 8; i++) usb_kbd[i] = i < len ? r[i] : 0;
    } else {
        for (UsbPad &p : usb_pads) {
            if (!p.on || p.addr != dev || p.idx != idx) continue;
            /* a failed request comes back empty (an idle pad NAKs instead:
             * no callback); failing on and on - it's gone (see
             * usb_pads_task) */
            if (len == 0) {
                if (!p.failing_since) p.failing_since = millis() | 1;
                continue;
            }
            p.failing_since = 0;
            uint16_t n = len < sizeof(p.raw) ? len : sizeof(p.raw);
            for (uint16_t i = 0; i < n; i++) p.raw[i] = r[i];
            p.raw_len = (uint8_t)n;
            if (p.fixed) {
                p.buttons = usb_parse_pad(r, len);
                p.numbers = 0;
            } else {
                uint32_t b, ad;
                uint16_t num;
                int32_t x = p.ax, y = p.ay;
                if (usb_parse_layout(p.lay, r, len, &b, &num, &ad, &x, &y)) {
                    p.buttons = b;
                    p.numbers = num;
                    p.adirs = ad;
                    p.ax = x;
                    p.ay = y;
                }
            }
        }
    }
    tuh_hid_receive_report(dev, idx);
}

/* The host stack runs on core 0, as in Adafruit's own Fruit Jam code
 * (Adafruit_Arcade_Machines, src/boards/fruitjam/usb_host_fruitjam.cpp).
 * It ran on core 1 at first, beside the HDMI output, and hung core 1 a
 * fraction of a second after start-up (the on-board hub is always there):
 * Pico-PIO-USB sends each packet from its 1 ms frame timer by starting the
 * DMA and then clearing the "done" flag, and an interrupt in between that
 * outlasts the packet wipes the flag - the transfer then waits for ever,
 * inside the timer interrupt. Core 1's HDMI scanline interrupt is the
 * highest priority there (it must be), so it did exactly that, and with
 * core 1 went the HDMI sound feed (and the emulator, paced by that sound).
 * Here the frame timer (TIMER0 alarm 2: Pico-PIO-USB's alarm pool, made on
 * the core that starts the host) gets the highest priority on core 0, so
 * nothing interrupts a transfer; core 1 has only the HDMI output again.
 * TinyUSB's waits while it sets a device up (debounce 150 ms, resets,
 * retries) spin on core 0: the menu / game holds for them, only then.
 * PIO 2 and DMA channel 11: clear of the HDMI driver's channels 0-3 and the
 * I2S's PIO. */
#include "hardware/irq.h"
#include "hardware/timer.h"
static bool usb_started = false;

static usb_pads_config_t usb_cfg;

/* Core 0, once (a8_input_init). */
extern "C" void usb_pads_begin(const usb_pads_config_t *c) {
    {
        usb_cfg = *c;
        if (usb_cfg.power_pin >= 0) {
            pinMode(usb_cfg.power_pin, OUTPUT);
            digitalWrite(usb_cfg.power_pin, usb_cfg.power_on);   /* the ports' 5 V */
        }
        pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
        cfg.pin_dp = usb_cfg.dp_pin;                      /* D- = D+ + 1 */
        /* (the PIO block, DMA channel: the caller's - Pico2Atari8's
         * A8_USB_PIO / A8_USB_DMA, 2 and 11) */
        cfg.pio_tx_num = usb_cfg.pio;
        cfg.pio_rx_num = usb_cfg.pio;
        cfg.tx_ch = usb_cfg.dma;
        Serial.printf("[usb] host on PIO %d, DMA channel %d%c", usb_cfg.pio, usb_cfg.dma, 10);
        usb_host.configure_pio_usb(1, &cfg);
        usb_host.begin(1);
        irq_set_priority(timer_hardware_alarm_get_irq_num(timer0_hw, 2), PICO_HIGHEST_IRQ_PRIORITY);
        usb_started = true;
    }
}

/* A flash write (a8_flash.cpp: unpacking a game, saving the settings) runs
 * with interrupts off for tens to hundreds of ms: no USB frames meanwhile,
 * and a USB device suspends after 3 ms of an idle bus. The SNES-style pad
 * then never woke properly (it kept repeating its last report: no button
 * worked in any game - each starts with an unpack - or after a save). So
 * after a write the ports' 5 V goes off for USB_POWER_OFF_MS and back on:
 * to the host, an unplug and a plug, and every device is set up again
 * (about half a second). Several writes in a row: one power-cycle, timed
 * from the last. */
static const uint32_t USB_POWER_OFF_MS = 300;
static bool usb_power_off = false;
static uint32_t usb_power_off_at = 0;

extern "C" void usb_pads_power_cycle(void) {
    if (!usb_started || usb_cfg.power_pin < 0) return;
    digitalWrite(usb_cfg.power_pin, !usb_cfg.power_on);
    usb_power_off = true;
    usb_power_off_at = millis();
}

/* A pad unplugged from the Fruit Jam's hub wasn't always noticed: the hub
 * reports a removal on its status endpoint, and TinyUSB's hub driver can
 * stop polling that (a failed re-arm, an enumeration that never ends) -
 * then no removal or new device was seen again. A pad that's gone fails
 * every request, so a pad whose reports fail for USB_GONE_MS on end is
 * taken as unplugged and the ports are power-cycled, as after a flash
 * write: the hub and the devices still there are set up afresh. */
static const uint32_t USB_GONE_MS = 500;

/* Core 0: the host stack's task (its callbacks run here), from the input
 * polling - the menus' (a8_input_menu) and the game's (a8_input_frame). */
extern "C" void usb_pads_task(void) {
    if (usb_power_off && millis() - usb_power_off_at >= USB_POWER_OFF_MS) {
        digitalWrite(usb_cfg.power_pin, usb_cfg.power_on);
        usb_power_off = false;
        Serial.printf("[usb] ports powered again%c", 10);
    }
    for (int n = 0; n < 2 && !usb_power_off; n++) {
        UsbPad &p = usb_pads[n];
        if (p.on && p.failing_since && millis() - p.failing_since >= USB_GONE_MS) {
            Serial.printf("[usb] gamepad %d stopped answering (unplugged?): the ports are reset%c", n + 1, 10);
            p.failing_since = 0;
            usb_pads_power_cycle();             /* (the same power-cycle) */
        }
    }
    if (usb_started) usb_host.task();
}

/* ---- the API (usb_pads.h) ------------------------------------------------ */
extern "C" bool usb_pads_connected(int n) { return n >= 0 && n < 2 && usb_pads[n].on; }
extern "C" uint32_t usb_pads_buttons(int n) { return usb_pads_connected(n) ? usb_pads[n].buttons : 0; }
extern "C" uint32_t usb_pads_stick_dirs(int n) { return usb_pads_connected(n) ? usb_pads[n].adirs : 0; }
extern "C" uint16_t usb_pads_numbers(int n) { return usb_pads_connected(n) ? usb_pads[n].numbers : 0; }

extern "C" bool usb_pads_analog(int n) {
    if (!usb_pads_connected(n)) return false;
    const UsbPad &p = usb_pads[n];
    return !p.fixed && p.lay.x.size && p.lay.x.lmax > p.lay.x.lmin;
}

/* An axis as -1000 .. +1000 of its range (0: centre). */
static int usb_axis_norm(const HidField &f, int32_t v) {
    if (!f.size || f.lmax <= f.lmin) return 0;
    const int32_t c2 = f.lmin + f.lmax;                  /* twice the centre */
    long n = ((long)v * 2 - c2) * 1000 / (f.lmax - f.lmin);
    return n < -1000 ? -1000 : n > 1000 ? 1000 : (int)n;
}

extern "C" int usb_pads_axis(int n, int axis) {
    if (n < 0 || n >= 2) return 0;
    const UsbPad &p = usb_pads[n];
    return axis ? usb_axis_norm(p.lay.y, p.ay) : usb_axis_norm(p.lay.x, p.ax);
}

extern "C" void usb_pads_axis_raw(int n, int axis, int32_t *value, int32_t *lmin, int32_t *lmax) {
    *value = *lmin = *lmax = 0;
    if (n < 0 || n >= 2) return;
    const UsbPad &p = usb_pads[n];
    const HidField &f = axis ? p.lay.y : p.lay.x;
    *value = axis ? p.ay : p.ax;
    *lmin = f.lmin;
    *lmax = f.lmax;
}

extern "C" void usb_pads_ids(int n, uint16_t *vid, uint16_t *pid) {
    const bool ok = n >= 0 && n < 2;
    *vid = ok ? usb_pads[n].vid : 0;
    *pid = ok ? usb_pads[n].pid : 0;
}

extern "C" int usb_pads_raw(int n, uint8_t raw[16]) {
    if (n < 0 || n >= 2) return 0;
    for (int i = 0; i < 16; i++) raw[i] = usb_pads[n].raw[i];
    return usb_pads[n].raw_len;
}

extern "C" bool usb_pads_keyboard(void) { return usb_kbd_on; }
extern "C" const uint8_t *usb_pads_kbd_report(void) { return usb_kbd; }
extern "C" bool usb_pads_kbd_has(uint8_t usage) {
    for (int i = 2; i < 8; i++) if (usb_kbd[i] == usage) return true;
    return false;
}
extern "C" uint32_t usb_pads_events(void) { return usb_events; }
