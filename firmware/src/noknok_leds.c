/*
 * noknok LEDs 16x Module - Firmware v2.0.0 (USBD / FSDEV)
 * MCU:   CH32V203G6U6 (rev 2.0 round board)
 * CLOCK: boots on HSI, manual switch to HSE(24MHz) x2 = 48 MHz (crystal-accurate).
 * USB:   USBD (FSDEV) controller via extralibs/usbd.c - the SAME controller the
 *        WCH bootloader uses (D+ pull-up via EXTEN_USBD_PU_EN).
 *        48 MHz core -> USBPRE=DIV1 -> 48 MHz USB clock.
 * LEDs:  16x SK6812 RGBW (GRBW, 32 bit/LED) on PA3 via TIM2_CH4 PWM + DMA1_Channel2.
 *        Hardware timer generates every bit - NO bit-banging, CPU not involved.
 * POWER: PB0 = LED-rail load switch (HIGH = on). PA3 must NEVER be high while the
 *        rail is off (level shifter U3 is always enabled -> would back-feed the
 *        dead rail through the LED DIN clamp). See led_rail_*() below.
 * STATUS: PB8 = status LED (active-high, also BOOT0) - held low by the app.
 * TIME:  TIM3 free-running 1 kHz (1 ms/tick) timebase for durations + animations.
 *
 * v2.0.0 changes (first firmware for the 16x RGBW board - NOT for the 8x board):
 *   - 16 LEDs, 4 channels (R G B W). Wire order to the LED = G R B W.
 *   - SK6812 timing: 1.25 us bit slot, T0H 0.31 us, T1H 0.60 us (dead centre of the
 *     SK6812 spec; the 8x value of 0.79 us was OUTSIDE it), reset >= 80 us.
 *   - Timer output-compare PRELOAD enabled: each DMA-written pulse width only takes
 *     effect at the next bit boundary, so no pulse is ever clipped/stretched mid-bit
 *     (the 8x driver wrote the live register - a marginal-waveform risk, cf. LED Button).
 *   - DMA priority VERY HIGH.
 *   - LED rail sequencing on PB0: rail off at boot, switched on (then 1 ms settle)
 *     before the first non-black frame, switched off again when the frame is all black.
 *   - Current cap: total LED current is estimated per frame and everything is scaled
 *     down if it would exceed LED_BUDGET_MA (450 mA = safe on a USB-2.0 port).
 *     (Step 3 will replace the constant with the CC-pin / VBUS-sense budget.)
 *   - NEW white-channel commands 0x11 / 0x12 / 0x14 / 0x18 / 0x21 (below). The old
 *     RGB commands keep working unchanged (white = 0), except 0x04 which now carries
 *     LED_COUNT x 3 = 48 bytes.
 *   - Parser timeout: a command still incomplete after 50 ms is discarded, so a host
 *     that sends a short 0x04 (old 8-LED driver) can't swallow the next command.
 *
 * v1.6 changes:
 *   - UNIQUE USB SERIAL: iSerialNumber is built at boot from the 96-bit chip UID
 *     (ESIG at 0x1FFFF7E8) -> 24 hex chars, so every module is uniquely
 *     identifiable on the USB bus (USB counterpart of the I2C UID).
 *   - 0x10 SET_LED: full per-LED control in one command (index/RGB/brightness/duration).
 *   - 0x20 PLAY_PRESET: 5 non-blocking animations (rainbow/breathe/chase/wipe/twinkle),
 *     fire-and-forget on the module (mirrors the buzzer's preset tunes).
 *
 * Command protocol (binary over USB serial; see module README):
 *   0x00                              all off
 *   0x01 R G B                        all one colour
 *   0x02 i R G B                      one LED
 *   0x03 B                            global brightness
 *   0x04 [48]                         16x RGB at once
 *   0x05                              show (re-latch current frame)
 *   0x10 i R G B BR durLo durHi       SET_LED: i(0xFF=all), brightness, duration ms (0=hold)
 *   0x20 preset speed R G B           PLAY_PRESET 1..6 (speed=ms/step, 0=default -
 *                                     EXCEPT preset 6 SUNDOWN, where speed instead
 *                                     means MINUTES, 0=default 30 - see below)
 *   --- white-channel variants (v2.0) - same meaning, plus a W byte ---
 *   0x11 R G B W                      all one colour
 *   0x12 i R G B W                    one LED
 *   0x14 [64]                         16x RGBW at once
 *   0x18 i R G B W BR durLo durHi     SET_LED with white
 *   0x21 preset speed R G B W         PLAY_PRESET with white (e.g. SUNDOWN in warm white)
 *   0xF0  -> 4E 4E 04                 identity
 *   0xB1  -> PROTO MAJ MIN PAT        GET_VERSION
 *   0xB0  -> (no reply)               ENTER_BOOTLOADER: hand off to the noknok USB bootloader (OTA)
 *
 * v1.8 changes (OTA-capable, bootloader-hosted):
 *   - App relinked at 0x2000 (app.ld) to run UNDER the noknok USB bootloader
 *     (module-USB-bootloader), with the top 16 B of RAM reserved as the handoff cell.
 *   - 0xB0 ENTER_BOOTLOADER: writes the handoff magic (0x6E6B4F54 @ 0x200027F0) +
 *     NVIC_SystemReset(); the bootloader catches it on reset and stays in USB
 *     flashing mode. Same proven pattern as the I2C modules.
 *   - Requires the bootloader flashed once via the BOOT0 jumper; thereafter the
 *     app updates over USB (tools/usb_flash.ps1 in module-USB-bootloader).
 *
 * v1.8.1 changes:
 *   - NEW PRESET 6 = SUNDOWN: a ONE-SHOT fade from full brightness to off in the
 *     caller-supplied colour (e.g. blue, for a "help my kid fall asleep" light).
 *     Unlike presets 1-5 (which loop forever until the next command), SUNDOWN
 *     runs once and then stops itself (anim=0) with the LEDs off.
 *   - EASING: quadratic ease-out, fraction = (1 - t/total)^2, computed in Q10
 *     fixed point (no FPU on this MCU). This is CONCAVE: brightness drops fast
 *     at the start and crawls the last stretch down to zero, not a linear ramp.
 *   - SPEED FIELD REPURPOSED FOR THIS PRESET ONLY: the existing `speed` byte in
 *     0x20 PLAY_PRESET means "ms per animation step" for presets 1-5, but a
 *     single byte of milliseconds cannot encode a useful total duration for a
 *     multi-minute fade. For preset 6 ONLY, `speed` is reinterpreted as MINUTES
 *     (1-255 min); 0 defaults to 30 min. This is a deliberate, flagged exception
 *     -- not a silent redefinition -- so a future preset must NOT assume `speed`
 *     always means ms/step without checking which preset it's serving.
 */

#include "ch32fun.h"
#include "usbd.h"
#include <string.h>

/* Firmware version - reported via 0xB1 GET_VERSION (noknok ecosystem standard,
 * command range 0xB0-0xBF). PROTOCOL_VERSION = shared noknok protocol level;
 * FW_VERSION_* = this firmware's semver (keep equal to the release tag). */
#define PROTOCOL_VERSION  0x01
#define FW_VERSION_MAJOR  2
#define FW_VERSION_MINOR  0
#define FW_VERSION_PATCH  0

/* ============================================================
 * Unique USB serial number (built from chip UID at boot)
 * Raw USB string descriptor: [bLength=50, bDescriptorType=3, 24x UTF-16LE hex].
 * usb_config.h references this via `extern uint8_t noknok_serial[]`.
 * ============================================================ */
uint8_t noknok_serial[2 + 24 * 2];

static void build_serial(void) {
    const volatile uint8_t *uid = (const volatile uint8_t *)0x1FFFF7E8; /* ESIG UNIID, 96-bit */
    static const char H[] = "0123456789ABCDEF";
    noknok_serial[0] = sizeof(noknok_serial);  /* bLength = 50 */
    noknok_serial[1] = 0x03;                    /* bDescriptorType = String */
    for (int i = 0; i < 12; i++) {              /* 12 UID bytes -> 24 hex chars */
        uint8_t b = uid[i];
        uint8_t hi = H[b >> 4], lo = H[b & 0x0F];
        noknok_serial[2 + (i * 2) * 2]     = hi;  noknok_serial[2 + (i * 2) * 2 + 1]     = 0x00;
        noknok_serial[2 + (i * 2 + 1) * 2] = lo;  noknok_serial[2 + (i * 2 + 1) * 2 + 1] = 0x00;
    }
}

/* ============================================================
 * Manual clock: HSI boot -> HSE 24 MHz x2 = 48 MHz
 * ============================================================ */
static int clock_to_hse_48(void) {
    RCC->CTLR |= RCC_HSEON;
    volatile uint32_t t = 1500000;
    while (t--) { if (RCC->CTLR & RCC_HSERDY) break; }
    if (!(RCC->CTLR & RCC_HSERDY)) return 0;
    FLASH->ACTLR = (FLASH->ACTLR & ~0x07u) | 0x01u;       /* 1 wait-state @48MHz */
    RCC->CFGR0 = RCC_HPRE_DIV1 | RCC_PPRE1_DIV1 | RCC_PPRE2_DIV1
               | RCC_PLLSRC | RCC_PLLMULL2;
    RCC->CTLR |= RCC_PLLON;
    t = 1500000;
    while (t--) { if (RCC->CTLR & RCC_PLLRDY) break; }
    if (!(RCC->CTLR & RCC_PLLRDY)) return 0;
    RCC->CFGR0 = (RCC->CFGR0 & ~(uint32_t)0x03) | RCC_SW_PLL;
    t = 1500000;
    while (t--) { if ((RCC->CFGR0 & RCC_SWS) == 0x08) break; }
    return ((RCC->CFGR0 & RCC_SWS) == 0x08);
}

/* ============================================================
 * TIM3 free-running 1 ms timebase
 * ============================================================ */
static void timebase_init(void) {
    RCC->APB1PCENR |= RCC_APB1Periph_TIM3;
    TIM3->PSC    = 48000 - 1;     /* 48 MHz / 48000 = 1 kHz -> 1 tick per ms */
    TIM3->ATRLR  = 0xFFFF;
    TIM3->CTLR1  = TIM_ARPE;
    TIM3->SWEVGR = TIM_UG;
    TIM3->CTLR1 |= TIM_CEN;
}
static inline uint16_t now_ms(void) { return (uint16_t)TIM3->CNT; }

/* ============================================================
 * Pins that must be safe from the very first instruction
 *   PA3 = LED data (TIM2_CH4) - driven LOW as plain GPIO until the timer owns it
 *   PB0 = LED-rail enable     - LOW = rail off
 *   PB8 = status LED          - LOW = off
 * ============================================================ */
static void pins_safe_init(void) {
    RCC->APB2PCENR |= RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB;
    GPIOA->BCR = (1u << 3);                                             /* PA3 low  */
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFUL << 12)) | (0x3UL << 12);     /* PA3 out PP 50MHz */
    GPIOB->BCR = (1u << 0) | (1u << 8);                                 /* PB0, PB8 low */
    GPIOB->CFGLR = (GPIOB->CFGLR & ~(0xFUL << 0)) | (0x3UL << 0);       /* PB0 out PP */
    GPIOB->CFGHR = (GPIOB->CFGHR & ~(0xFUL << 0)) | (0x3UL << 0);       /* PB8 out PP */
}

/* ============================================================
 * SK6812 RGBW driver - TIM2_CH4 PWM + DMA1_Channel2, 48 MHz timer clock
 *
 * One timer period = one LED bit (60 ticks = 1.25 us). On every timer update
 * the DMA copies the next pulse width into CH4's compare register. With the
 * compare PRELOAD on, that value only becomes active at the NEXT update, so each
 * bit is a whole, clean pulse. The CPU only builds the buffer and waits.
 * ============================================================ */
#define LED_COUNT      16
#define LED_CH         4                /* bytes per LED on the wire: G R B W   */
#define WS_PERIOD      60               /* 60 x 20.83 ns = 1.25 us bit slot     */
#define WS_T0H         15               /* 0.31 us  (SK6812 spec 0.30 +- 0.15)  */
#define WS_T1H         29               /* 0.60 us  (SK6812 spec 0.60 +- 0.15)  */
#define WS_RESET_BITS  80               /* 80 x 1.25 us = 100 us low (>= 80 us) */
#define WS_BUF_LEN     (LED_COUNT * LED_CH * 8 + WS_RESET_BITS)

static uint16_t ws_dma_buf[WS_BUF_LEN] __attribute__((aligned(4)));

static void ws_build_buf(const uint8_t *grbw) {
    uint16_t *p = ws_dma_buf;
    for (int i = 0; i < LED_COUNT * LED_CH; i++) {
        uint8_t byte = grbw[i];
        for (int bit = 7; bit >= 0; bit--)
            *p++ = (byte >> bit) & 1 ? WS_T1H : WS_T0H;
    }
    memset(p, 0, WS_RESET_BITS * sizeof(uint16_t));   /* trailing low = latch/reset */
}

static void ws_init(void) {
    RCC->APB1PCENR |= RCC_APB1Periph_TIM2;
    RCC->AHBPCENR  |= RCC_AHBPeriph_DMA1;     /* re-enable DMA1 (USBDSetup overwrote AHBPCENR) */

    TIM2->CTLR1   = 0;
    TIM2->PSC     = 0;
    TIM2->ATRLR   = WS_PERIOD - 1;
    TIM2->CH4CVR  = 0;                        /* 0 = output stays low */
    /* CH4: PWM mode 1 (OC4M=110) + output-compare PRELOAD (OC4PE) */
    TIM2->CHCTLR2 = (TIM2->CHCTLR2 & ~0xFF00) | ((0x60 | 0x08) << 8);
    TIM2->CCER   |= TIM_CC4E;                 /* active-high output */
    TIM2->CTLR1  |= TIM_ARPE;
    TIM2->DMAINTENR |= TIM_UDE;               /* DMA request on every update */

    DMA1_Channel2->CFGR  = 0;
    DMA1_Channel2->PADDR = (uint32_t)&TIM2->CH4CVR;
    DMA1_Channel2->CFGR  =
        DMA_CFGR1_DIR | DMA_CFGR1_MINC | DMA_CFGR1_PSIZE_0 |
        DMA_CFGR1_MSIZE_0 | DMA_CFGR1_PL;     /* PL = 11 = very high priority */

    TIM2->CTLR1 |= TIM_CEN;                   /* free-running; output low while CCR=0 */

    /* Hand PA3 to the timer only now - the timer output is already low (CCR=0),
     * so the pin never glitches high. AF push-pull 50 MHz. */
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFUL << 12)) | (0xBUL << 12);
}

static void ws_show(const uint8_t *grbw) {
    ws_build_buf(grbw);
    DMA1_Channel2->CFGR  &= ~DMA_CFGR1_EN;
    DMA1->INTFCR = DMA1_IT_TC2;
    DMA1_Channel2->CNTR  = WS_BUF_LEN;
    DMA1_Channel2->MADDR = (uint32_t)ws_dma_buf;
    DMA1_Channel2->CFGR |= DMA_CFGR1_EN;      /* starts at the next timer update */
    while (!(DMA1->INTFR & DMA1_IT_TC2));     /* ~0.9 ms for 16 LEDs */
    DMA1->INTFCR = DMA1_IT_TC2;
    /* The last buffer entries are zeros, so CCR is left at 0 -> line stays low. */
}

/* ============================================================
 * LED rail (PB0) - never drive data into an unpowered LED
 * ============================================================ */
static uint8_t rail_on = 0;

static void led_rail_on(void) {
    if (rail_on) return;
    GPIOB->BSHR = (1u << 0);                  /* PB0 high -> Q2/Q1 switch the rail on */
    Delay_Ms(1);                              /* soft start ~160 us + LED power-on reset */
    rail_on = 1;
}
static void led_rail_off(void) {
    if (!rail_on) return;
    GPIOB->BCR = (1u << 0);                   /* data line is already low (CCR = 0) */
    rail_on = 0;
}

/* ============================================================
 * LED state
 * ============================================================ */
static uint8_t fr[LED_COUNT], fg[LED_COUNT], fb[LED_COUNT], fw[LED_COUNT];
static uint8_t brightness = 255;
static inline uint8_t scale(uint8_t v) { return (uint16_t)v * brightness / 255; }

/* Current cap. Rough model: a channel at 255 draws ~LED_MA_PER_CH mA, linear.
 * If the frame's total estimate exceeds LED_BUDGET_MA, every channel is scaled
 * down by the same factor (colours keep their ratio). Budget becomes dynamic
 * (CC pins + VBUS sense) in step 3. */
#define LED_MA_PER_CH   20
#define LED_BUDGET_MA   450
#define LED_BUDGET_SUM  ((uint32_t)LED_BUDGET_MA * 255u / LED_MA_PER_CH)

static void show(void) {
    uint8_t grbw[LED_COUNT * LED_CH];
    uint32_t sum = 0;
    for (int i = 0; i < LED_COUNT; i++) {
        grbw[i*4+0] = scale(fg[i]); grbw[i*4+1] = scale(fr[i]);
        grbw[i*4+2] = scale(fb[i]); grbw[i*4+3] = scale(fw[i]);
        sum += grbw[i*4+0] + grbw[i*4+1] + grbw[i*4+2] + grbw[i*4+3];
    }
    if (sum == 0) {                           /* all black: blank the LEDs, then cut power */
        if (rail_on) ws_show(grbw);
        led_rail_off();
        return;
    }
    if (sum > LED_BUDGET_SUM)
        for (int i = 0; i < LED_COUNT * LED_CH; i++)
            grbw[i] = (uint8_t)((uint32_t)grbw[i] * LED_BUDGET_SUM / sum);
    led_rail_on();
    ws_show(grbw);
}
static void set_all_w(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    for (int i = 0; i < LED_COUNT; i++) { fr[i]=r; fg[i]=g; fb[i]=b; fw[i]=w; }
}
static void set_all(uint8_t r, uint8_t g, uint8_t b) { set_all_w(r, g, b, 0); }

/* ============================================================
 * Per-LED timed-off (0x10 duration) + animation engine (0x20)
 * ============================================================ */
static uint16_t led_deadline[LED_COUNT];   /* TIM3 ms value at which the LED turns off */
static uint8_t  led_timed[LED_COUNT];      /* 1 = a deadline is armed for this LED */

static uint8_t  anim = 0;                   /* 0 = none, 1..6 = active preset */
static uint8_t  anim_r, anim_g, anim_b, anim_w; /* preset base colour (w = white) */
static uint16_t anim_step_ms = 40;          /* ms between animation steps */
static uint16_t anim_last;                  /* last step timestamp */
static uint16_t anim_phase;                 /* animation phase counter (also used as
                                              * SUNDOWN's elapsed-step counter) */
static uint16_t anim_total_steps;           /* SUNDOWN only: steps for a full fade */

static uint32_t rng_state = 0xA5A5F00D;
static inline uint32_t rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

/* Stop any running animation and clear all pending timed-off deadlines. Called
 * by every immediate command so manual control always wins. */
static void cancel_dynamic(void) {
    anim = 0;
    for (int i = 0; i < LED_COUNT; i++) led_timed[i] = 0;
}

/* NeoPixel-style colour wheel: hue 0..255 -> RGB. */
static void wheel(uint8_t pos, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (pos < 85)       { *r = 255 - pos*3; *g = pos*3;       *b = 0; }
    else if (pos < 170) { pos -= 85;  *r = 0; *g = 255 - pos*3; *b = pos*3; }
    else                { pos -= 170; *r = pos*3; *g = 0;     *b = 255 - pos*3; }
}

static void anim_render(void) {
    switch (anim) {
        case 1: /* rainbow rotate */
            for (int i = 0; i < LED_COUNT; i++)
                { wheel((uint8_t)(anim_phase + i * (256 / LED_COUNT)), &fr[i], &fg[i], &fb[i]); fw[i] = 0; }
            anim_phase += 4;
            break;
        case 2: { /* breathe (triangle on base colour) */
            uint8_t t = anim_phase & 0xFF;
            uint8_t lvl = (t < 128) ? (uint8_t)(t * 2) : (uint8_t)((255 - t) * 2);
            for (int i = 0; i < LED_COUNT; i++) {
                fr[i] = (uint16_t)anim_r * lvl / 255;
                fg[i] = (uint16_t)anim_g * lvl / 255;
                fb[i] = (uint16_t)anim_b * lvl / 255;
                fw[i] = (uint16_t)anim_w * lvl / 255;
            }
            anim_phase += 4;
            break;
        }
        case 3: /* theatre chase (every 3rd LED, base colour) */
            for (int i = 0; i < LED_COUNT; i++) {
                uint8_t on = (((i + anim_phase) % 3) == 0);
                fr[i] = on ? anim_r : 0; fg[i] = on ? anim_g : 0; fb[i] = on ? anim_b : 0; fw[i] = on ? anim_w : 0;
            }
            anim_phase += 1;
            break;
        case 4: { /* colour wipe (fill up then clear, repeat) */
            uint8_t k = anim_phase % (LED_COUNT + 1);
            for (int i = 0; i < LED_COUNT; i++) {
                uint8_t on = (i < k);
                fr[i] = on ? anim_r : 0; fg[i] = on ? anim_g : 0; fb[i] = on ? anim_b : 0; fw[i] = on ? anim_w : 0;
            }
            anim_phase += 1;
            break;
        }
        case 5: /* twinkle (random sparkle in base colour) */
            for (int i = 0; i < LED_COUNT; i++) {
                if ((rng() & 0x07) == 0) { fr[i] = anim_r; fg[i] = anim_g; fb[i] = anim_b; fw[i] = anim_w; }
                else                     { fr[i] = 0; fg[i] = 0; fb[i] = 0; fw[i] = 0; }
            }
            anim_phase += 1;
            break;
        case 6: { /* SUNDOWN: one-shot fade from full brightness to off over
                   * anim_total_steps steps, EASED (quadratic ease-out: fast
                   * dim at the start, slow crawl to zero at the end) - see the
                   * SUNDOWN comment block above anim_render() for the formula
                   * and why speed means "minutes" here, not "ms/step". */
            uint16_t t = anim_phase;                       /* elapsed steps */
            if (t >= anim_total_steps) {
                set_all(0, 0, 0);
                anim = 0;                                   /* one-shot: stop, stay off */
                break;
            }
            /* frac = (1 - t/total)^2, computed in Q10 fixed point (0..1024).
             * remain10 = (1 - t/total) scaled to 0..1024 (16x16->32 bit, no FPU). */
            uint32_t remain10 = ((uint32_t)(anim_total_steps - t) << 10) / anim_total_steps;
            uint32_t frac10   = (remain10 * remain10) >> 10;  /* square, rescale back to Q10 */
            for (int i = 0; i < LED_COUNT; i++) {
                fr[i] = (uint8_t)(((uint32_t)anim_r * frac10) >> 10);
                fg[i] = (uint8_t)(((uint32_t)anim_g * frac10) >> 10);
                fb[i] = (uint8_t)(((uint32_t)anim_b * frac10) >> 10);
                fw[i] = (uint8_t)(((uint32_t)anim_w * frac10) >> 10);
            }
            anim_phase += 1;
            break;
        }
        default: return;
    }
    show();
}

/* ============================================================
 * Command parser
 * ============================================================ */
typedef enum { PARSE_IDLE, PARSE_WAIT } ParseState;
static ParseState ps = PARSE_IDLE;
static uint8_t cmd_byte=0, cmd_buf[LED_COUNT * 4], cmd_need=0, cmd_got=0;
static uint16_t cmd_start;          /* ms timestamp of the command byte */

/* ------------------------------------------------------------
 * 0xB0 ENTER_BOOTLOADER - hand off to the noknok USB bootloader for OTA.
 * Writes the agreed magic to the no-init handoff cell (top 16 B of RAM, kept
 * out of the linker's RAM region by app.ld so it survives a warm reset), then
 * a system reset. The bootloader runs first on every reset, sees the magic,
 * and stays in USB flashing mode. Same proven pattern as the I2C modules, and
 * NVIC_SystemReset is the reset we validated reboots cleanly on this chip.
 * NOTE: this app must be built with app.ld (linked at 0x2000 + handoff cell).
 * ------------------------------------------------------------ */
#define HANDOFF_CELL    (*(volatile uint32_t *)0x200027F0u)
#define ENTER_BL_MAGIC  0x6E6B4F54u   /* 'nkOT' - must match the bootloader */

static void enter_bootloader(void) {
    HANDOFF_CELL = ENTER_BL_MAGIC;
    NVIC_SystemReset();
    while (1) { }   /* unreachable */
}

/* Apply one LED's colour (idx 0xFF = all). */
static void put_led(uint8_t idx, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    if (idx == 0xFF) set_all_w(r, g, b, w);
    else if (idx < LED_COUNT) { fr[idx]=r; fg[idx]=g; fb[idx]=b; fw[idx]=w; }
}

/* SET_LED (0x10 RGB / 0x18 RGBW): i R G B [W] brightness durLo durHi */
static void cmd_set_led(const uint8_t *a, uint8_t has_w) {
    cancel_dynamic();
    uint8_t idx = a[0];
    uint8_t w   = has_w ? a[4] : 0;
    const uint8_t *t = a + (has_w ? 5 : 4);          /* brightness, durLo, durHi */
    brightness = t[0];
    uint16_t dur = (uint16_t)t[1] | ((uint16_t)t[2] << 8);
    uint16_t deadline = now_ms() + dur;
    put_led(idx, a[1], a[2], a[3], w);
    if (dur) {
        if (idx == 0xFF) for (int i=0;i<LED_COUNT;i++){ led_deadline[i]=deadline; led_timed[i]=1; }
        else if (idx < LED_COUNT) { led_deadline[idx]=deadline; led_timed[idx]=1; }
    }
    show();
}

/* PLAY_PRESET (0x20 RGB / 0x21 RGBW): preset speed R G B [W] */
static void cmd_play_preset(const uint8_t *a, uint8_t has_w) {
    uint8_t p = a[0];
    if (p < 1 || p > 6) return;
    for (int i=0;i<LED_COUNT;i++) led_timed[i]=0;  /* drop pending timed-offs */
    anim = p;
    anim_r = a[2]; anim_g = a[3]; anim_b = a[4]; anim_w = has_w ? a[5] : 0;
    anim_phase = 0;
    if (p == 6) {
        /* SUNDOWN reinterprets `speed` as MINUTES (not ms/step) -
         * a 1-byte ms/step can't encode a 30-minute total duration.
         * speed=0 -> default 30 min. Fixed cadence: 1 render/second
         * (smooth enough for a slow fade, keeps anim_total_steps
         * within a uint16_t for up to ~18 hours). */
        uint8_t minutes = a[1] ? a[1] : 30;
        anim_step_ms = 1000;
        anim_total_steps = (uint16_t)((uint32_t)minutes * 60u);
    } else {
        anim_step_ms = a[1] ? a[1] : 40;
    }
    anim_last = now_ms();
    anim_render();   /* draw first frame immediately */
}

static void execute(void) {
    const uint8_t *a = cmd_buf;
    switch (cmd_byte) {
        case 0x00: cancel_dynamic(); set_all(0,0,0); show(); break;
        case 0x01: cancel_dynamic(); set_all(a[0],a[1],a[2]); show(); break;
        case 0x11: cancel_dynamic(); set_all_w(a[0],a[1],a[2],a[3]); show(); break;
        case 0x02:
            cancel_dynamic();
            if (a[0] < LED_COUNT) { put_led(a[0], a[1], a[2], a[3], 0); show(); }
            break;
        case 0x12:
            cancel_dynamic();
            if (a[0] < LED_COUNT) { put_led(a[0], a[1], a[2], a[3], a[4]); show(); }
            break;
        case 0x03: brightness = a[0]; show(); break;
        case 0x04:
            cancel_dynamic();
            for (int i=0;i<LED_COUNT;i++) put_led(i, a[i*3+0], a[i*3+1], a[i*3+2], 0);
            show(); break;
        case 0x14:
            cancel_dynamic();
            for (int i=0;i<LED_COUNT;i++) put_led(i, a[i*4+0], a[i*4+1], a[i*4+2], a[i*4+3]);
            show(); break;
        case 0x05: show(); break;

        case 0x10: cmd_set_led(a, 0); break;
        case 0x18: cmd_set_led(a, 1); break;
        case 0x20: cmd_play_preset(a, 0); break;
        case 0x21: cmd_play_preset(a, 1); break;

        case 0xF0: { static const uint8_t id[3]={0x4E,0x4E,0x04}; USBD_SendEndpoint(3, (uint8_t*)id, 3); break; }
        case 0xB1: { static const uint8_t ver[4]={PROTOCOL_VERSION,FW_VERSION_MAJOR,FW_VERSION_MINOR,FW_VERSION_PATCH}; USBD_SendEndpoint(3, (uint8_t*)ver, 4); break; }
        case 0xB0: enter_bootloader(); break;  /* ENTER_BOOTLOADER (no reply; resets) */
    }
}

/* Payload length per command; -1 = unknown command (byte ignored). */
static int cmd_len(uint8_t b) {
    switch (b) {
        case 0x00: return 0;
        case 0x01: return 3;
        case 0x02: return 4;
        case 0x03: return 1;
        case 0x04: return LED_COUNT * 3;       /* 48 */
        case 0x05: return 0;
        case 0x10: return 7;
        case 0x11: return 4;
        case 0x12: return 5;
        case 0x14: return LED_COUNT * 4;       /* 64 */
        case 0x18: return 8;
        case 0x20: return 5;
        case 0x21: return 6;
        case 0xF0: return 0;
        case 0xB1: return 0;
        case 0xB0: return 0;
        default:   return -1;
    }
}

#define CMD_TIMEOUT_MS  50   /* an incomplete command older than this is dropped */

static void process_byte(uint8_t b) {
    if (ps == PARSE_IDLE) {
        int n = cmd_len(b);
        if (n < 0) return;
        cmd_byte = b; cmd_got = 0; cmd_need = (uint8_t)n;
        cmd_start = now_ms();
        if (cmd_need==0) execute(); else ps=PARSE_WAIT;
    } else {
        cmd_buf[cmd_got++] = b;
        if (cmd_got >= cmd_need) { execute(); ps=PARSE_IDLE; }
    }
}
/* ============================================================
 * USBD callbacks
 * ============================================================ */
/* RX from host (EP2) is delivered here by poll_input(). */
void handle_usbd_input(int numbytes, uint8_t *data) {
    for (int i = 0; i < numbytes; i++) process_byte(data[i]);
}

/* ch32fun's USB-printf backend calls this symbol; route it to USBD. */
int USBFS_SendEndpointNEW(int endp, uint8_t *data, int len, int copy) {
    (void)copy;
    return USBD_SendEndpoint(endp, data, len);
}

/* ============================================================
 * Main
 * ============================================================ */
int main(void) {
    pins_safe_init();         /* PA3 data low, LED rail off, status LED off - FIRST */
    clock_to_hse_48();        /* crystal-accurate 48 MHz first */
    build_serial();           /* fill iSerialNumber from chip UID before enumeration */
    USBDSetup();              /* FSDEV USB device (asserts EXTEN_USBD_PU_EN) */
    timebase_init();          /* TIM3 1 ms tick */
    ws_init();                /* re-enables DMA1 after USBDSetup AHBPCENR overwrite */

    /* Boot flash: dim warm white (W channel only) 300 ms */
    set_all_w(0,0,0,20); show();
    Delay_Ms(300);
    set_all(0,0,0); show();

    while (1) {
        poll_input();         /* dispatches received bytes to handle_usbd_input */

        uint16_t t = now_ms();

        /* Drop a command whose payload never completed (e.g. an old host sending
         * the 8-LED 0x04). Otherwise it would swallow the next command. */
        if (ps == PARSE_WAIT && (uint16_t)(t - cmd_start) > CMD_TIMEOUT_MS) ps = PARSE_IDLE;

        /* Service per-LED timed-off deadlines (0x10 duration). */
        uint8_t need_show = 0;
        for (int i = 0; i < LED_COUNT; i++) {
            if (led_timed[i] && (int16_t)(t - led_deadline[i]) >= 0) {
                fr[i] = fg[i] = fb[i] = fw[i] = 0; led_timed[i] = 0; need_show = 1;
            }
        }
        if (need_show) show();

        /* Service the active animation (0x20). */
        if (anim && (int16_t)(t - anim_last) >= (int16_t)anim_step_ms) {
            anim_last += anim_step_ms;
            anim_render();
        }

        Delay_Ms(1);
    }
}
