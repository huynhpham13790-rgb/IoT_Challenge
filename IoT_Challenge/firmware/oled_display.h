/* ============================================================================
 *  oled_display.h — 1.3" 128x64 I2C OLED at the bedside (Smart IV, ICTU team)
 *
 *  Ported from the standalone heart-rate bench build (IoT_Challenge-main,
 *  2026-08-11), which drove the same panel from a MAX30102-only firmware. The
 *  panel controller handling, the init sequence and the page-refresh scheme
 *  are that code's; what it displays was rewritten for this firmware, which
 *  knows a great deal more than a BPM figure (SpO2, flow against the
 *  prescribed target, and why the bed is alarming).
 *
 *  WHY A SCREEN ON THE DEVICE AT ALL, given the ward console shows all of
 *  this already: the console is at the nurses' station and depends on the
 *  whole chain behind it — Zigbee, the Pi, zigbee2mqtt, the gateway, the HIS
 *  Server. The nurse standing at the bed, adjusting the roller clamp, is
 *  looking at the pump, not down the corridor; and when the chain is down
 *  (it has been) the bedside is the only place the readings still exist.
 *
 *  WIRING (shares the I2C bus with the MAX30102, exactly as the bench build
 *  did — the panel is a second address on the same two wires):
 *      VDD/VCC -> 3V3        (NOT 5V: the EFR32 I2C pins are 3.3V logic)
 *      GND     -> GND
 *      SCK/SCL -> PC05       (same pin as the MAX30102 SCL)
 *      SDA     -> PC07       (same pin as the MAX30102 SDA)
 *  "SCK" silkscreened on an I2C OLED means the I2C clock. It goes to SCL —
 *  not to any SPI pin.
 * ========================================================================== */
#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#define OLED_I2C_ADDRESS      0x3CU
/* Otherwise identical modules strap SA0 high and answer at 0x3D instead. */
#define OLED_I2C_ADDRESS_ALT  0x3DU
#define OLED_WIDTH            128U
#define OLED_HEIGHT           64U

/* The driver never touches the I2C bus itself: it is handed a write function
 * by whoever owns the bus (sensor_hub.c, which already bit-bangs PC05/PC07
 * for the MAX30102). One owner keeps the two devices from interleaving
 * half-finished transactions on the shared wires.
 *
 * `control` is the OLED's own control byte: 0x00 = the bytes that follow are
 * commands, 0x40 = they are display RAM. */
typedef bool (*oled_i2c_write_fn)(void *context, uint8_t address,
                                  uint8_t control, const uint8_t *data,
                                  uint8_t length);

typedef struct {
  oled_i2c_write_fn write;
  void *context;
} oled_bus_t;

typedef struct {
  oled_bus_t bus;
  uint8_t address;
  uint8_t framebuffer[OLED_WIDTH * OLED_HEIGHT / 8U];
  bool initialized;
} oled_display_t;

/* What the bed currently is. Everything the screen shows comes from this one
 * struct, so the display can never disagree with the alarm logic that filled
 * it in — it has no thresholds of its own. */
typedef struct {
  bool     hr_valid;      /* HR channel is CH_OK (a real, fresh reading) */
  uint16_t hr_bpm;
  bool     spo2_valid;    /* SpO2 channel is CH_OK */
  uint16_t spo2_pct;
  bool     flow_valid;    /* flow channel is CH_OK */
  int16_t  flow_pct;      /* flow as % of the doctor's target, 100 = on target */
  bool     alarm;         /* any alarm reason is active right now */
  /* Individual reasons, shown as the bottom banner. Only the most severe is
   * displayed: 128x64 at a readable size fits one line, and a nurse who reads
   * one line reliably is worth more than four lines nobody reads. The console
   * still lists every reason at once. */
  bool reason_spo2;
  bool reason_hr;
  bool reason_flow;
  bool reason_missing;
  bool reason_ai;
} oled_vitals_t;

/* Probes 0x3C then 0x3D, runs the panel init sequence and paints the splash.
 * Returns false if neither address ACKs — the caller keeps running without a
 * screen rather than failing to boot. */
bool oled_display_init(oled_display_t *display, const oled_bus_t *bus);

/* Paints the bedside screen. Returns false if the panel stopped ACKing (cable
 * pulled, bus glitch), which the caller uses to schedule a re-init.
 *
 * Cheap to call every second: it redraws only when the rendered content
 * actually changes, so a steady bed sends nothing at all over I2C. */
bool oled_display_show_vitals(oled_display_t *display,
                              const oled_vitals_t *vitals);

/* "SMART IV / ICTU" splash, shown at boot until the first reading arrives. */
bool oled_display_show_splash(oled_display_t *display);

#endif /* OLED_DISPLAY_H */
