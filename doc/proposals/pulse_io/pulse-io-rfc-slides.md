---
marp: true
theme: default
paginate: true
style: |
  section {
    font-size: 24px;
    padding: 40px 50px;
    justify-content: flex-start;
  }
  h1 { font-size: 40px; }
  h2 { font-size: 30px; margin-bottom: 0.3em; }
  pre {
    font-size: 15px;
    line-height: 1.25;
    margin: 0.3em 0;
  }
  code { font-size: 15px; }
  table { font-size: 20px; }
  li { margin: 0.1em 0; }
---

# pulse_io

### A vendor-neutral subsystem for timed-edge IO

RFC: zephyrproject-rtos/zephyr #109586
Status: OPEN - Architecture Review

---

## The problem

Many MCUs have hardware to make and read timed edges on one pin.
Each vendor names it differently:

- Espressif RMT
- RP2040 PIO
- NXP FlexIO
- STM32 TIM + DMA
- Nordic PWM + PPI + GPIOTE

Same job, five names. No shared Zephyr API.

---

## Who needs it

- Addressable LEDs (WS2812, SK6812, APA102)
- IR remote send / receive (NEC, RC5, RC6, SIRC)
- Stepper motor pulse trains
- Frequency and duty measurement (tacho, anemometer, ToF)
- OneWire and bit-banged protocols

Today every client driver is tied to the hardware.
WS2812 alone has three backends: I2S, SPI, GPIO.

---

## Why a subsystem

We tried pushing the Espressif RMT driver into `drivers/misc`.

It was rejected: it contains HAL calls, which are not expected
for a general driver in that folder.

That is the wall that motivates a real subsystem with
vendor backends.

---

## The key idea: SYMBOL and CELL

A channel picks one mode at configure time.

| Mode | What you send | Good for |
|---|---|---|
| **SYMBOL** | `{level, duration}` pairs | IR, OneWire, stepper ramps |
| **CELL** | fixed-period cells | WS2812, varying-duty PWM |

**Why two, not one:** SYMBOL-only forces fixed-period hardware to
bit-blast (RAM grows with `period / tick`); CELL-only kills any
case with freely varying edges. `caps.modes` advertises both, so a
portable client prefers CELL and falls back to SYMBOL.

**CELL detail:** period is set once (`cfg.cell_period_ticks`). The
backend's `cell_duty_bits` says what each cell carries: `0` = a
`level` (bit shaping, WS2812), `>0` = a `duty` (varying PWM, servo).

---

## The API

Every backend exposes the same calls:

```
get_capabilities
channel_get / channel_release
channel_configure
transmit_sync / transmit_async
receive_sync  / receive_async
stop
```

- Blocking and non-blocking
- Send and receive
- No vendor names in the public header (same style as spi.h)

---

## How a client uses it

Same five steps on every backend:

```c
pulse_io_channel_get(dev, 0, &ch);
pulse_io_channel_configure(dev, ch, &cfg);
pulse_io_transmit_sync(dev, ch, &req, K_MSEC(100));
pulse_io_stop(dev, ch);
pulse_io_channel_release(dev, ch);
```

The next slides show real clients.

---

## Sample: stepper ramp (SYMBOL mode)

Accelerate: each step shorter than the last.
This is why durations are per-symbol, not fixed.

```c
struct pulse_io_config cfg = {
    .mode = PULSE_IO_MODE_SYMBOL,   /* arbitrary edge lengths */
    .dir  = PULSE_IO_DIR_TX,
    .resolution_hz = 1000000,       /* 1 us ticks */
};
pulse_io_channel_configure(dev, ch, &cfg);

struct pulse_symbol ramp[2 * 100];  /* 100 steps */
uint32_t gap = 800;                 /* start slow: 800 us */

for (int i = 0; i < 100; i++) {
    ramp[2 * i]     = (struct pulse_symbol){ .level = 1, .duration = 100 };
    ramp[2 * i + 1] = (struct pulse_symbol){ .level = 0, .duration = gap };
    if (gap > 200) {
        gap -= 6;                   /* speed up each step */
    }
}

struct pulse_io_tx_req req = { .symbols = ramp, .count = 2 * 100 };
pulse_io_transmit_sync(dev, ch, &req, K_MSEC(100));
```

Each step = one high pulse + one gap. The shrinking gap is
the acceleration.

---

## Sample: WS2812 (codec + SYMBOL)

Codec expands each byte into pulse symbols via a bit template.
Show green for one second, then switch to red.

```c
struct pulse_io_config cfg = {
    .mode = PULSE_IO_MODE_SYMBOL,   /* encoder emits symbols */
    .dir  = PULSE_IO_DIR_TX,
    .resolution_hz = 1000000,
};
pulse_io_channel_configure(dev, ch, &cfg);

/* zero bit = short high, one bit = long high */
struct pulse_io_bit_template ws2812 = {
    .zero = {{1, T0H}, {0, T0L}},
    .one  = {{1, T1H}, {0, T1L}},
    .msb_first = true,
};

struct pulse_symbol buf[24 * 2];
size_t n;

/* WS2812 byte order is Green, Red, Blue */
uint8_t green[] = { 0xFF, 0x00, 0x00 };
uint8_t red[]   = { 0x00, 0xFF, 0x00 };

pulse_io_encode_bytes(&ws2812, green, 3, buf, ARRAY_SIZE(buf), &n);
pulse_io_transmit_sync(dev, ch, &(struct pulse_io_tx_req){
    .symbols = buf, .count = n }, K_MSEC(10));

k_sleep(K_SECONDS(1));               /* show green for 1 s */

pulse_io_encode_bytes(&ws2812, red, 3, buf, ARRAY_SIZE(buf), &n);
pulse_io_transmit_sync(dev, ch, &(struct pulse_io_tx_req){
    .symbols = buf, .count = n }, K_MSEC(10));
```

---

## Sample: ask first, then act

A portable client checks caps before configuring.

```c
struct pulse_io_caps caps;

pulse_io_get_capabilities(dev, &caps);

if (caps.modes & PULSE_IO_MODE_CELL) {
    cfg.mode = PULSE_IO_MODE_CELL;   /* cheaper for WS2812 */
} else {
    cfg.mode = PULSE_IO_MODE_SYMBOL; /* fall back */
}
```

Same client binary runs on RMT, PIO, FlexIO, ...

---

## Sample: servo / dimming (CELL duty)

Fixed 20 ms frame, only the high-time changes per frame.

```c
struct pulse_io_config cfg = {
    .mode = PULSE_IO_MODE_CELL,
    .dir  = PULSE_IO_DIR_TX,
    .cell_period_ticks = US_TO_TICKS(20000), /* 50 Hz frame */
};

/* 1.0 ms = 0 deg, 1.5 ms = 90 deg, 2.0 ms = 180 deg */
struct pulse_cell angles[] = {
    { .duty = US_TO_TICKS(1000) },
    { .duty = US_TO_TICKS(1500) },
    { .duty = US_TO_TICKS(2000) },
};

struct pulse_io_tx_req req = { .cells = angles, .count = 3 };
pulse_io_transmit_sync(dev, ch, &req, K_MSEC(100));
```

---

## Sample: DShot ESC (CELL duty)

Drone motor protocol: a 16-bit frame, each bit a fixed period,
bit value = duty inside that period.

```c
/* frame = 11-bit throttle + 1-bit telemetry + 4-bit CRC */
uint16_t val = (throttle << 1) | telemetry;
uint8_t  crc = (val ^ (val >> 4) ^ (val >> 8)) & 0x0F;
uint16_t frame = (val << 4) | crc;

/* DShot600: bit period ~1.67 us; "1" ~75% duty, "0" ~37% */
struct pulse_cell bits[16];

for (int i = 0; i < 16; i++) {
    bool one = frame & (0x8000 >> i);          /* MSB first */
    bits[i].duty = one ? DUTY_75 : DUTY_37;
}

struct pulse_io_tx_req req = { .cells = bits, .count = 16 };
pulse_io_transmit_sync(dev, ch, &req, K_MSEC(1));
```

---

## Sample: IR receive (SYMBOL, RX)

Capture a remote frame. Edges have wildly different lengths.

```c
struct pulse_io_config cfg = {
    .mode = PULSE_IO_MODE_SYMBOL,
    .dir  = PULSE_IO_DIR_RX,
    .resolution_hz = 1000000,            /* 1 us ticks */
    .rx_idle_threshold_ticks = 10000,    /* frame ends after 10 ms idle */
    .rx_carrier_demod = true,            /* strip 38 kHz carrier */
};

struct pulse_symbol frame[128];
struct pulse_io_rx_req req = { .symbols = frame, .capacity = 128 };
size_t count;

pulse_io_receive_sync(dev, ch, &req, &count, K_FOREVER);
/* decode NEC / RC5 from frame[0..count] */
```

---

## Sample: frequency / tacho (RX)

Measure pulse timing - fan RPM, anemometer, flow meter.

```c
struct pulse_symbol edges[64];
struct pulse_io_rx_req req = { .symbols = edges, .capacity = 64 };
size_t count;

pulse_io_receive_sync(dev, ch, &req, &count, K_MSEC(500));

/* period = high-time + low-time of one cycle, in ticks */
uint32_t period = edges[0].duration + edges[1].duration;
uint32_t hz = cfg.resolution_hz / period;
```

---

## Sample: OneWire temp sensor (TX + RX)

Read a DS18B20: a reset pulse, then read-slots on one line.
Same channel does both, in SYMBOL mode at 1 us ticks.

```c
/* 1. reset: pull low 480 us, release; sensor answers with a
 *    presence pulse we capture on the same line */
struct pulse_symbol reset = { .level = 0, .duration = 480 };
struct pulse_symbol presence[4];
size_t n;

pulse_io_transmit_sync(dev, ch, &(struct pulse_io_tx_req){
    .symbols = &reset, .count = 1 }, K_MSEC(2));

pulse_io_receive_sync(dev, ch, &(struct pulse_io_rx_req){
    .symbols = presence, .capacity = 4 }, &n, K_MSEC(2));
/* a low pulse ~60-240 us in presence[] = sensor is there */
```

---

## Sample: real ws2812 driver

A new `led_strip` backend next to ws2812_spi.c / _i2s.c.
Only the sink changes - same color loop as the SPI driver.

```c
#define DT_DRV_COMPAT worldsemi_ws2812_pulse_io

#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/pulse_io.h>

struct ws2812_pulse_io_cfg {
    const struct device *pulse_io;     /* backend: RMT, PIO, ... */
    uint8_t channel_idx;
    struct pulse_symbol *sym_buf;      /* pixels*colors*8*2 */
    size_t sym_capacity;
    struct pulse_io_bit_template tmpl; /* WS2812 0/1 timings */
    uint8_t num_colors;
    const uint8_t *color_mapping;
    size_t length;
    uint16_t reset_delay;
};

struct ws2812_pulse_io_data {
    struct pulse_io_channel *chan;     /* filled at init */
};

static int ws2812_pulse_io_update_rgb(const struct device *dev,
                                      struct led_rgb *px, size_t n)
{
    const struct ws2812_pulse_io_cfg *cfg = dev->config;
    struct ws2812_pulse_io_data *data = dev->data;
    struct pulse_symbol *out = cfg->sym_buf;
    size_t total = 0;

    for (size_t i = 0; i < n; i++) {
        uint8_t grb[3] = { px[i].g, px[i].r, px[i].b };
        size_t produced;

        pulse_io_encode_bytes(&cfg->tmpl, grb, 3,
                              out, cfg->sym_capacity - total, &produced);
        out += produced;  total += produced;
    }

    struct pulse_io_tx_req req = { .symbols = cfg->sym_buf, .count = total };
    pulse_io_transmit_sync(cfg->pulse_io, data->chan, &req, K_MSEC(100));
    k_usleep(cfg->reset_delay);   /* latch, same as SPI driver */
    return 0;
}

static int ws2812_pulse_io_init(const struct device *dev)
{
    const struct ws2812_pulse_io_cfg *cfg = dev->config;
    struct ws2812_pulse_io_data *data = dev->data;
    struct pulse_io_caps caps;
    int rc;

    if (!device_is_ready(cfg->pulse_io)) {
        return -ENODEV;
    }
    rc = pulse_io_get_capabilities(cfg->pulse_io, &caps);
    if (rc < 0 || !(caps.modes & PULSE_IO_MODE_SYMBOL) || !caps.supports_tx) {
        return -ENOTSUP;
    }
    rc = pulse_io_channel_get(cfg->pulse_io, cfg->channel_idx, &data->chan);
    if (rc < 0) {
        return rc;
    }
    struct pulse_io_config ch = {
        .mode = PULSE_IO_MODE_SYMBOL, .dir = PULSE_IO_DIR_TX,
        .resolution_hz = 1000000,
    };
    return pulse_io_channel_configure(cfg->pulse_io, data->chan, &ch);
}

static DEVICE_API(led_strip, ws2812_pulse_io_api) = {
    .update_rgb = ws2812_pulse_io_update_rgb,
    .length     = ws2812_pulse_io_length,
};

#define NPIX(idx) DT_INST_PROP(idx, chain_length)
#define NCOL(idx) DT_INST_PROP_LEN(idx, color_mapping)
#define SYMS(idx) (NPIX(idx) * NCOL(idx) * 8 * 2)  /* worst case */

#define WS2812_PULSE_IO_DEVICE(idx)                                       \
    static struct pulse_symbol ws2812_pi_##idx##_buf[SYMS(idx)];          \
    static const uint8_t ws2812_pi_##idx##_map[] =                        \
        DT_INST_PROP(idx, color_mapping);                                 \
    static struct ws2812_pulse_io_data ws2812_pi_##idx##_data;            \
    static const struct ws2812_pulse_io_cfg ws2812_pi_##idx##_cfg = {     \
        .pulse_io     = DEVICE_DT_GET(DT_INST_PHANDLE(idx, pulse_ios)),    \
        .channel_idx  = DT_INST_PHA(idx, pulse_ios, channel),             \
        .sym_buf      = ws2812_pi_##idx##_buf,                            \
        .sym_capacity = SYMS(idx),                                        \
        .tmpl = { .zero = {{1, DT_INST_PROP(idx, t0h_ns)/1000},           \
                           {0, DT_INST_PROP(idx, t0l_ns)/1000}},          \
                  .one  = {{1, DT_INST_PROP(idx, t1h_ns)/1000},           \
                           {0, DT_INST_PROP(idx, t1l_ns)/1000}},          \
                  .msb_first = true },                                    \
        .num_colors    = NCOL(idx),                                       \
        .color_mapping = ws2812_pi_##idx##_map,                           \
        .length        = NPIX(idx),                                       \
        .reset_delay   = DT_INST_PROP(idx, reset_delay),                  \
    };                                                                    \
    DEVICE_DT_INST_DEFINE(idx, ws2812_pulse_io_init, NULL,                \
                          &ws2812_pi_##idx##_data, &ws2812_pi_##idx##_cfg,\
                          POST_KERNEL, CONFIG_LED_STRIP_INIT_PRIORITY,    \
                          &ws2812_pulse_io_api);

DT_INST_FOREACH_STATUS_OKAY(WS2812_PULSE_IO_DEVICE)
```

---

## What the app sees: nothing changes

The new backend is just another sibling:

```
drivers/led_strip/
    ws2812_spi.c
    ws2812_i2s.c
    ws2812_gpio.c
    ws2812_pulse_io.c   <- new, talks to pulse_io
```

App code is the same generic led_strip call:

```c
struct led_rgb green = { .r = 0, .g = 0xFF, .b = 0 };
led_strip_update_rgb(strip, &green, 1);
```

Because it binds to `pulse_io`, the same file runs on RMT
today and on PIO / FlexIO when those backends land.

---

## The plan

- **PR1:** API + RMT reference backend + WS2812 client
  (proves the API is real)
- **PR2+:** other vendor backends, each added by its owner

---

## Feedback already in

- **bjarki-andreasen:** SPI (MOSI-only) can be a portable software
  backend - a universal fallback.
- **joelguittet:** Reviewed, looks good, ready to implement and
  fold into PR #101448.
- **tannewt (CircuitPython):** Same API exists there (RMT-backed,
  used for IR). They only have mode 1; they keep WS2812 separate
  because per-bit duration bloats memory. Our CELL mode fixes that.

---

## Decisions for today

1. Lock the name `pulse_io`, or pick another?
2. Add a generic SPI fallback backend now, or defer?
3. Use PR #101448 as the implementation vehicle?

---

# Thank you

RFC: zephyrproject-rtos/zephyr #109586

Questions?
