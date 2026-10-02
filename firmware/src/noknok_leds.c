/*
 * noknok LEDs 16x Module - Firmware v2.2.1 (USBD / FSDEV)
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
 * v2.0.1 changes (bench fix, 29 Sep 2026):
 *   - LOSSLESS USB RECEIVE. usbd.c's printf RX path (64 B buffer, discard-oldest)
 *     dropped the command byte of a 65-byte 0x14 frame; the colour bytes were then
 *     parsed as commands (0x00 = ALL OFF -> LEDs went dark). Now our own 256 B ring +
 *     USB NAK flow control (see 'USB receive path'). FUNCONF_USE_USBPRINTF = 0.
 *
 * v2.0.2 changes (bench calibration, 29 Sep 2026):
 *   - Current model 20 -> 12 mA per channel at 255 (measured, see 'Current cap').
 *     Same 450 mA LED budget, but no longer ~2x over-cautious: white die at 100 %
 *     now runs uncapped; only near-full all-channel frames are limited.
 *
 * v2.1.0 changes (29 Sep 2026) - power governor, first half of 'step 3':
 *   - ADC1 reads VBUS (PA0), CC1/CC2 (PA1/PA2), die temperature (ch16) and VREFINT
 *     (ch17, for true mV) 10x per second.
 *   - LED budget = source limit from the CC pins (default 450 / 1.5 A + 3 A: 1000 mA)
 *     x VBUS factor (dims while VBUS sags, cuts the LED rail below VBUS_CUT_MV)
 *     x thermal factor (dims from 55 C, LED rail off at 75 C until < 65 C).
 *     THRESHOLDS ARE FIRST GUESSES until a heat-up curve and VBUS readings exist.
 *   - 0x30 GET_STATUS / 0x3F GET_SENSOR_RAW. The app can show the temperature
 *     (read-only) from 0x30.
 *
 * v2.1.1 (29 Sep 2026): FIX - thermal derating never reached a static frame (the
 *   re-draw compared the budget per 100 ms tick, and slow derating never moved 2 %
 *   in one tick). Now compared against the budget the LEDs were last drawn with.
 *
 * v2.2.0 (29 Sep 2026): OWN MODULE TYPE 0x06. 0xF0 now answers 4E 4E 06 (was 04,
 *   shared with the 8x board). Hosts that only know 0x04 no longer mistake it for an 8x.
 *
 * v2.2.1 (2 Oct 2026, DEV-28): ws_show() refuses to send LED data unless PB0's output
 *   register says the LED rail is on - the PA3/PB0 rule is now a hard check in the
 *   driver, not only a property of how show() is written.
 *   FIX: CC1/CC2 are read through the board's 3.3k/1.8k Rd divider (PA1/PA2 see
 *   CC x 1.8/5.1); now scaled back to the real CC voltage. Before, a 3 A USB-C source
 *   (CC 1.63 V) read 0.57 V and was treated as default USB (budget stuck at 450 mA).
 *   GET_STATUS CC1/CC2 now report the real line voltage.
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
 *   0xF0  -> 4E 4E 06                 identity (0x06 = LEDs 16x; the 8x board is 0x04)
 *   0xB1  -> PROTO MAJ MIN PAT        GET_VERSION
 *   0xB0  -> (no reply)               ENTER_BOOTLOADER: hand off to the noknok USB bootloader (OTA)
 *   0x30  -> 16 bytes                 GET_STATUS: temperature, VBUS, CC, LED budget/current (v2.1)
 *   0x3F  -> 18 bytes                 GET_SENSOR_RAW: raw ADC + calibration word (bench/debug)
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

/* noknok module type code (Ecosystem software/enumeration.md 'Module type codes').
 * 0x06 = noknok LEDs 16x - its OWN board type, decided 29 Sep 2026. The 8x RGB board
 * is 0x04. USB modules share one app PID (0x4E4E), so this byte in the 0xF0 identity
 * reply is what tells the brain which driver to use. */
#define MODULE_TYPE       0x06
#define FW_VERSION_MAJOR  2
#define FW_VERSION_MINOR  2
#define FW_VERSION_PATCH  1

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
    /* HARD INVARIANT (rev 2.0 board, DEV-28): never clock data out on PA3 while the
     * LED rail is off. U3 (level shifter) is always enabled, so a high on PA3 would
     * push ~4.9 V into the DIN of unpowered LEDs and back-feed the dead rail. Checked
     * against the PB0 OUTPUT REGISTER itself, not a software flag, so no caller or
     * future code path can get around it. */
    if (!(GPIOB->OUTDR & (1u << 0))) return;
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

/* Current cap. Model: a channel at 255 draws ~LED_MA_PER_CH mA, linear.
 * If the frame's total estimate exceeds LED_BUDGET_MA, every channel is scaled
 * down by the same factor (colours keep their ratio). Budget becomes dynamic
 * (CC pins + VBUS sense) in step 3.
 * CALIBRATED on the bench 29 Sep 2026 (KWS-X1 inline on USB-C, rev 2.0 board):
 *   LEDs off (rail off) = 0.03 W; 16x white die @255 = 0.95 W -> ~11.5 mA/channel;
 *   capped RGBW mix = ~9.7 mA/channel. 12 = worst measured, rounded up.
 *   => all 64 channels at full would be ~0.7 A uncapped (not the 1.3 A first assumed). */
#define LED_MA_PER_CH   12
#define LED_BUDGET_DEFAULT_MA  450            /* USB default power (500 mA port) */

/* Set by the power governor (v2.1.0, see 'Sensors + power governor'):
 *   led_budget_ma = source limit (CC pins) x VBUS headroom x thermal factor.
 *   0 = LED rail must stay OFF (thermal or VBUS cut) - the frame is kept and comes
 *   back automatically when the governor releases. */
static uint16_t led_budget_ma = LED_BUDGET_DEFAULT_MA;
static uint16_t led_est_ma;                   /* estimated LED current of the frame on the LEDs */
static uint8_t  frame_lit;                    /* 1 = the requested frame is not all black */
static uint16_t shown_budget_ma = LED_BUDGET_DEFAULT_MA;  /* budget the LEDs were last drawn with */

static void show(void) {
    uint8_t grbw[LED_COUNT * LED_CH];
    uint32_t sum = 0;
    for (int i = 0; i < LED_COUNT; i++) {
        grbw[i*4+0] = scale(fg[i]); grbw[i*4+1] = scale(fr[i]);
        grbw[i*4+2] = scale(fb[i]); grbw[i*4+3] = scale(fw[i]);
        sum += grbw[i*4+0] + grbw[i*4+1] + grbw[i*4+2] + grbw[i*4+3];
    }
    frame_lit = (sum != 0);
    shown_budget_ma = led_budget_ma;
    if (sum == 0 || led_budget_ma == 0) {     /* black, or governor cut: blank, then cut power */
        if (rail_on) { memset(grbw, 0, sizeof grbw); ws_show(grbw); }
        led_rail_off();
        led_est_ma = 0;
        return;
    }
    uint32_t limit = (uint32_t)led_budget_ma * 255u / LED_MA_PER_CH;
    if (sum > limit) {
        for (int i = 0; i < LED_COUNT * LED_CH; i++)
            grbw[i] = (uint8_t)((uint32_t)grbw[i] * limit / sum);
        sum = limit;
    }
    led_est_ma = (uint16_t)(sum * LED_MA_PER_CH / 255u);
    led_rail_on();
    ws_show(grbw);
}
static void set_all_w(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    for (int i = 0; i < LED_COUNT; i++) { fr[i]=r; fg[i]=g; fb[i]=b; fw[i]=w; }
}
static void set_all(uint8_t r, uint8_t g, uint8_t b) { set_all_w(r, g, b, 0); }

/* ============================================================
 * Sensors (ADC1) + power governor (v2.1.0)
 *
 * Inputs, read 10x per second:
 *   PA0  = VBUS via 22k/22k divider (x2)
 *   PA1  = CC1, PA2 = CC2 - ANALOG IN, NO pull-up/down (a pull would sit in
 *          parallel with the 5.1k Rd and break USB-C source detection)
 *   ch16 = internal temperature sensor, ch17 = VREFINT (1.20 V). VREFINT gives
 *          the real VDDA, so every reading is in true mV, not "fraction of 3.3 V".
 *
 * Output: led_budget_ma = source limit (CC) x VBUS factor x thermal factor.
 *
 * ⚠ THRESHOLDS ARE FIRST GUESSES (29 Sep 2026), to be set from a measured
 *   heat-up curve and from VBUS readings compared against a KWS-X1. Deliberately
 *   on the cautious side. Read everything back with 0x30 / 0x3F.
 * ============================================================ */
#define VREFINT_MV          1200
#define TS_CAL_ADDR         0x1FFFF720u   /* WCH factory word: [15:0] mV, [31:16] deg C (to be verified) */
#define TS_SLOPE_X10        43            /* 4.3 mV per deg C, voltage FALLS as it gets warmer */
#define TS_V25_MV_DEFAULT   1430          /* datasheet typical, used if the factory word looks wrong */

/* Thermal (die temperature, 0.1 deg C) */
#define T_DERATE_START_DC   550           /* start dimming at 55.0 C */
#define T_DERATE_FULL_DC    700           /* at 70.0 C dimmed to T_DERATE_MIN_Q8 */
#define T_DERATE_MIN_Q8     64            /* 25 % (Q8: 256 = 100 %) */
#define T_CUT_DC            750           /* LED power OFF at 75.0 C ... */
#define T_RESUME_DC         650           /* ... back on below 65.0 C */

/* VBUS (mV) - where PA0 sits relative to the input diode is not yet verified */
#define VBUS_DERATE_MV      4400          /* below: step brightness down */
#define VBUS_RECOVER_MV     4550          /* above: step back up */
#define VBUS_CUT_MV         4100          /* below: LED power OFF (keep MCU + USB alive) */

/* CC pin voltage with the 5.1k Rd (USB-C spec ranges) */
#define CC_1A5_MV           660           /* >= : source offers 1.5 A */
#define CC_3A_MV            1230          /* >= : source offers 3.0 A */
#define LED_BUDGET_1A5_MA   1000
#define LED_BUDGET_3A_MA    1000          /* the board tops out ~0.7 A anyway; thermal rules */

static uint16_t s_vdda_mv, s_vbus_mv, s_cc1_mv, s_cc2_mv;
static int16_t  s_temp_dc;                /* filtered die temperature, 0.1 deg C */
static uint16_t s_raw_ts, s_raw_vref, s_raw_vbus, s_raw_cc1, s_raw_cc2;
static uint16_t s_src_budget_ma = LED_BUDGET_DEFAULT_MA;
static uint16_t s_vbus_q8 = 256, s_therm_q8 = 256;   /* Q8 factors, 256 = 100 % */
static uint8_t  s_therm_cut, s_vbus_cut;
static uint8_t  s_temp_valid;
static int16_t  s_ts_ref_mv, s_ts_ref_c;  /* calibration actually in use */
static uint8_t  s_ts_cal_ok;              /* 1 = factory word used, 0 = datasheet default */

static void adc_init(void) {
    RCC->APB2PCENR |= RCC_APB2Periph_ADC1 | RCC_APB2Periph_GPIOA;
    RCC->CFGR0 = (RCC->CFGR0 & ~RCC_ADCPRE) | RCC_ADCPRE_DIV4;       /* 48 / 4 = 12 MHz */
    GPIOA->CFGLR &= ~(0xFFFUL << 0);                                  /* PA0..PA2 analog, no pulls */
    ADC1->SAMPTR2 = 0x3FFFFFFF;           /* ch0-9   : 239.5 cycles (~20 us) */
    ADC1->SAMPTR1 = 0x00FFFFFF;           /* ch10-17 : 239.5 cycles (temp sensor needs >= 17 us) */
    ADC1->RSQR1   = 0;                    /* one conversion per start */
    ADC1->CTLR2   = ADC_ADON | ADC_TSVREFE | ADC_EXTSEL | ADC_EXTTRIG;   /* software trigger */
    Delay_Ms(1);                          /* ADC + temp sensor settle */
    ADC1->CTLR2 |= ADC_RSTCAL; for (int t = 100000; (ADC1->CTLR2 & ADC_RSTCAL) && t; t--);
    ADC1->CTLR2 |= ADC_CAL;    for (int t = 100000; (ADC1->CTLR2 & ADC_CAL) && t; t--);

    uint32_t cal = *(volatile uint32_t *)TS_CAL_ADDR;
    int16_t mv = (int16_t)(cal & 0xFFFF), c = (int16_t)(cal >> 16);
    s_ts_cal_ok = (mv > 1000 && mv < 1800 && c >= 0 && c <= 60);
    s_ts_ref_mv = s_ts_cal_ok ? mv : TS_V25_MV_DEFAULT;
    s_ts_ref_c  = s_ts_cal_ok ? c  : 25;
}

static uint16_t adc_read(uint8_t ch) {    /* 8-sample average, 12-bit */
    uint32_t acc = 0;
    ADC1->RSQR3 = ch;
    for (int n = 0; n < 8; n++) {
        ADC1->CTLR2 |= ADC_SWSTART;
        for (int t = 20000; !(ADC1->STATR & ADC_EOC) && t; t--);
        acc += ADC1->RDATAR & 0x0FFF;     /* reading RDATAR clears EOC */
    }
    return (uint16_t)(acc / 8);
}
static inline uint16_t raw_to_mv(uint16_t raw) { return (uint16_t)((uint32_t)raw * s_vdda_mv / 4095u); }

/* CC sense on the rev 2.0 board is NOT the CC pin itself: the 5.1k Rd is split into
 * 3.3k (R20/R21, connector side) + 1.8k (R1/R2, to GND) and PA1/PA2 tap the middle
 * through 4.7k (R18/R19, no current flows -> no error). So the ADC sees CC x 1.8/5.1.
 * Scale back to the real CC line voltage so the USB-C spec thresholds apply.
 * (Found 2 Oct 2026: a 3 A USB-C charger, CC1 = 1.63 V on the KWS-X1, read as
 * 0.57 V and was treated as "default USB" -> budget stuck at 450 mA.) */
#define CC_DIV_TOP_OHM      3300
#define CC_DIV_BOT_OHM      1800
static inline uint16_t cc_line_mv(uint16_t raw) {
    return (uint16_t)((uint32_t)raw_to_mv(raw) * (CC_DIV_TOP_OHM + CC_DIV_BOT_OHM) / CC_DIV_BOT_OHM);
}

static void governor_tick(void) {
    /* --- measure --- */
    s_raw_vref = adc_read(17);
    s_vdda_mv  = s_raw_vref ? (uint16_t)((uint32_t)VREFINT_MV * 4095u / s_raw_vref) : 3300;
    s_raw_vbus = adc_read(0);  s_vbus_mv = raw_to_mv(s_raw_vbus) * 2;
    s_raw_cc1  = adc_read(1);  s_cc1_mv  = cc_line_mv(s_raw_cc1);
    s_raw_cc2  = adc_read(2);  s_cc2_mv  = cc_line_mv(s_raw_cc2);
    s_raw_ts   = adc_read(16);
    int32_t ts_mv = raw_to_mv(s_raw_ts);
    int16_t t_now = (int16_t)(s_ts_ref_c * 10 - ((ts_mv - s_ts_ref_mv) * 100) / TS_SLOPE_X10);
    if (!s_temp_valid) { s_temp_dc = t_now; s_temp_valid = 1; }
    else s_temp_dc += (t_now - s_temp_dc) / 8;          /* ~1 s smoothing */

    /* --- source limit from the CC pins (the higher one is the connected one) --- */
    uint16_t cc = s_cc1_mv > s_cc2_mv ? s_cc1_mv : s_cc2_mv;
    s_src_budget_ma = cc >= CC_3A_MV  ? LED_BUDGET_3A_MA
                    : cc >= CC_1A5_MV ? LED_BUDGET_1A5_MA
                    : LED_BUDGET_DEFAULT_MA;           /* default USB, or no CC (legacy cable) */

    /* --- VBUS headroom: step down 10 % per tick while sagging, recover 5 % per tick --- */
    if (s_vbus_mv < VBUS_CUT_MV)          s_vbus_cut = 1;
    else if (s_vbus_mv > VBUS_RECOVER_MV) s_vbus_cut = 0;
    if (s_vbus_mv < VBUS_DERATE_MV)       s_vbus_q8 = s_vbus_q8 > 77 ? s_vbus_q8 - 26 : 51;   /* floor 20 % */
    else if (s_vbus_mv > VBUS_RECOVER_MV) s_vbus_q8 = s_vbus_q8 < 243 ? s_vbus_q8 + 13 : 256;

    /* --- thermal: linear derate, hard cut with hysteresis --- */
    if (s_temp_dc >= T_CUT_DC)        s_therm_cut = 1;
    else if (s_temp_dc < T_RESUME_DC) s_therm_cut = 0;
    if (s_temp_dc <= T_DERATE_START_DC)     s_therm_q8 = 256;
    else if (s_temp_dc >= T_DERATE_FULL_DC) s_therm_q8 = T_DERATE_MIN_Q8;
    else s_therm_q8 = (uint16_t)(256 - (uint32_t)(256 - T_DERATE_MIN_Q8) * (s_temp_dc - T_DERATE_START_DC)
                                       / (T_DERATE_FULL_DC - T_DERATE_START_DC));

    /* --- combine; re-apply to a lit static frame if the budget moved ---
     * Compare against the budget the LEDs were last DRAWN with, not the previous
     * tick: thermal derating moves the budget ~1 mA per tick, which never crossed a
     * per-tick 2 % threshold, so a static frame was never dimmed (bench 29 Sep 2026). */
    uint16_t b = (s_therm_cut || s_vbus_cut) ? 0
               : (uint16_t)((uint32_t)s_src_budget_ma * s_vbus_q8 / 256u * s_therm_q8 / 256u);
    led_budget_ma = b;
    uint16_t a = shown_budget_ma;
    uint16_t d = b > a ? b - a : a - b;
    if (frame_lit && (d * 50u > a || (b == 0) != (a == 0))) show();   /* > 2 % from drawn, or cut/uncut */
}

/* 0x30 GET_STATUS -> 16 bytes, little-endian. Stable layout - the app reads it.
 *   [0]     layout version (1)
 *   [1..2]  int16  die temperature, 0.1 deg C
 *   [3..4]  uint16 VBUS mV        [5..6] CC1 mV       [7..8] CC2 mV
 *   [9..10] uint16 LED budget mA (0 = LED power cut by the governor)
 *   [11..12]uint16 estimated LED current mA of the current frame
 *   [13]    flags: b0 LED rail on, b1 thermal cut, b2 VBUS cut, b3 temp from factory cal
 *   [14]    thermal factor %      [15] VBUS factor %                          */
static void send_status(void) {
    uint8_t r[16];
    r[0] = 1;
    r[1] = (uint8_t)s_temp_dc;        r[2]  = (uint8_t)((uint16_t)s_temp_dc >> 8);
    r[3] = (uint8_t)s_vbus_mv;        r[4]  = (uint8_t)(s_vbus_mv >> 8);
    r[5] = (uint8_t)s_cc1_mv;         r[6]  = (uint8_t)(s_cc1_mv >> 8);
    r[7] = (uint8_t)s_cc2_mv;         r[8]  = (uint8_t)(s_cc2_mv >> 8);
    r[9] = (uint8_t)led_budget_ma;    r[10] = (uint8_t)(led_budget_ma >> 8);
    r[11] = (uint8_t)led_est_ma;      r[12] = (uint8_t)(led_est_ma >> 8);
    r[13] = (rail_on ? 1 : 0) | (s_therm_cut ? 2 : 0) | (s_vbus_cut ? 4 : 0) | (s_ts_cal_ok ? 8 : 0);
    r[14] = (uint8_t)((uint32_t)s_therm_q8 * 100u / 256u);
    r[15] = (uint8_t)((uint32_t)s_vbus_q8 * 100u / 256u);
    USBD_SendEndpoint(3, r, 16);
}

/* 0x3F GET_SENSOR_RAW (bench/debug) -> 18 bytes, little-endian:
 *   [0..3] factory temp-cal word @0x1FFFF720 as read, [4..5] VDDA mV,
 *   [6..7] raw TS, [8..9] raw VREFINT, [10..11] raw VBUS, [12..13] raw CC1,
 *   [14..15] raw CC2, [16..17] int16 unfiltered-equivalent: calibration ref mV in use */
static void send_sensor_raw(void) {
    uint8_t r[18];
    uint32_t cal = *(volatile uint32_t *)TS_CAL_ADDR;
    uint16_t v[7] = { s_vdda_mv, s_raw_ts, s_raw_vref, s_raw_vbus, s_raw_cc1, s_raw_cc2, (uint16_t)s_ts_ref_mv };
    r[0] = (uint8_t)cal; r[1] = (uint8_t)(cal >> 8); r[2] = (uint8_t)(cal >> 16); r[3] = (uint8_t)(cal >> 24);
    for (int i = 0; i < 7; i++) { r[4 + i*2] = (uint8_t)v[i]; r[5 + i*2] = (uint8_t)(v[i] >> 8); }
    USBD_SendEndpoint(3, r, 18);
}

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

        case 0xF0: { static const uint8_t id[3]={0x4E,0x4E,MODULE_TYPE}; USBD_SendEndpoint(3, (uint8_t*)id, 3); break; }
        case 0xB1: { static const uint8_t ver[4]={PROTOCOL_VERSION,FW_VERSION_MAJOR,FW_VERSION_MINOR,FW_VERSION_PATCH}; USBD_SendEndpoint(3, (uint8_t*)ver, 4); break; }
        case 0xB0: enter_bootloader(); break;  /* ENTER_BOOTLOADER (no reply; resets) */
        case 0x30: send_status(); break;       /* GET_STATUS (temperature, power) */
        case 0x3F: send_sensor_raw(); break;   /* GET_SENSOR_RAW (bench/debug) */
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
        case 0x30: return 0;
        case 0x3F: return 0;
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
 * USB receive path (CDC OUT = EP2) - lossless, with USB flow control
 *
 * WHY this is ours and not usbd.c's (v2.0.0): usbd.c's built-in receive path
 * (FUNCONF_USE_USBPRINTF) has a 64-byte buffer that silently DISCARDS THE
 * OLDEST BYTES when a second packet arrives before the main loop polls. A
 * 65-byte 0x14 frame (64 + 1 byte packets) lost its command byte, and the
 * colour bytes were then parsed as commands (0x00 = ALL OFF). Found on the
 * bench 29 Sep 2026.
 *
 * Here: a 256-byte ring filled from the USB interrupt. If fewer than 64 bytes
 * (one full packet) are free after a packet, the USB interrupt is masked. The
 * endpoint hardware then NAKs further OUT packets on its own, so the host just
 * retries - it can never overrun us. The main loop drains the ring and unmasks.
 * ============================================================ */
#define RX_RING_SIZE   256                     /* power of two */
#define RX_PKT_MAX     64                      /* full-speed bulk max packet */
static uint8_t           rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head;              /* written by the USB IRQ only */
static volatile uint16_t rx_tail;              /* written by the main loop only */
static volatile uint8_t  rx_paused;            /* 1 = USB IRQ masked for flow control */

/* usbd.c callback (USB IRQ context): data arrived on an OUT endpoint. */
void HandleDataOut(struct _USBState *ctx, int endp, uint8_t *data, int len) {
    if (endp == 0) { ctx->USBD_SetupReqLen = 0; return; }   /* EP0 data stage: ACK */
    if (endp != 2) return;
    uint16_t h = rx_head;
    for (int i = 0; i < len; i++) rx_ring[h++ & (RX_RING_SIZE - 1)] = data[i];
    rx_head = h;
    if (RX_RING_SIZE - (uint16_t)(h - rx_tail) < RX_PKT_MAX) {
        NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);   /* no room for another packet -> NAK */
        rx_paused = 1;
    }
}

/* usbd.c callback: IN request on an endpoint - nothing queued here. */
int HandleInRequest(struct _USBState *ctx, int endp, uint8_t *data, int len) {
    (void)ctx; (void)endp; (void)data; (void)len;
    return 0;
}

/* usbd.c callback: CDC class requests (line coding etc.) - accept them all.
 * Identical to usbd.c's FUNCONF_USE_USBPRINTF version. */
int HandleSetupCustom(struct _USBState *ctx, int setup_code) {
    int ret = -1;
    if (ctx->USBD_SetupReqType & USB_REQ_TYP_CLASS) {
        switch (setup_code) {
            case CDC_SET_LINE_CODING:
            case CDC_SET_LINE_CTLSTE:
            case CDC_SEND_BREAK:      ret = (ctx->USBD_SetupReqLen) ? ctx->USBD_SetupReqLen : -1; break;
            case CDC_GET_LINE_CODING: ret = ctx->USBD_SetupReqLen; break;
            default:                  ret = 0; break;
        }
    } else {
        ret = 0;   /* go to STALL */
    }
    return ret;
}

/* Main loop: take everything received so far, release flow control, then parse.
 * The IRQ is unmasked BEFORE parsing so replies (0xF0/0xB1 on EP3) can complete. */
static void rx_poll(void) {
    uint8_t buf[RX_RING_SIZE];
    uint16_t t = rx_tail, n = (uint16_t)(rx_head - t);
    for (uint16_t i = 0; i < n; i++) buf[i] = rx_ring[(t + i) & (RX_RING_SIZE - 1)];
    rx_tail = t + n;
    if (rx_paused) { rx_paused = 0; NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn); }
    for (uint16_t i = 0; i < n; i++) process_byte(buf[i]);
}

/* ============================================================
 * Main
 * ============================================================ */
static uint16_t gov_last;

int main(void) {
    pins_safe_init();         /* PA3 data low, LED rail off, status LED off - FIRST */
    clock_to_hse_48();        /* crystal-accurate 48 MHz first */
    build_serial();           /* fill iSerialNumber from chip UID before enumeration */
    USBDSetup();              /* FSDEV USB device (asserts EXTEN_USBD_PU_EN) */
    timebase_init();          /* TIM3 1 ms tick */
    ws_init();                /* re-enables DMA1 after USBDSetup AHBPCENR overwrite */
    adc_init();               /* VBUS / CC / temperature sensing */
    governor_tick();          /* valid LED budget before the first frame */

    /* Boot flash: dim warm white (W channel only) 300 ms */
    set_all_w(0,0,0,20); show();
    Delay_Ms(300);
    set_all(0,0,0); show();
#ifdef BENCH_FULL_ON_BOOT
    /* TEST BUILD ONLY (never release): light everything at full after boot so the
     * governor can be checked on a power-only source (charger, no USB host). */
    Delay_Ms(1000);
    set_all_w(255,255,255,255); show();
#endif

    while (1) {
        rx_poll();            /* parse everything received over USB since last pass */

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

        /* Power governor: temperature, VBUS, CC -> LED budget, 10x per second. */
        if ((uint16_t)(t - gov_last) >= 100) { gov_last = t; governor_tick(); }

        /* Service the active animation (0x20). */
        if (anim && (int16_t)(t - anim_last) >= (int16_t)anim_step_ms) {
            anim_last += anim_step_ms;
            anim_render();
        }

        Delay_Ms(1);
    }
}
