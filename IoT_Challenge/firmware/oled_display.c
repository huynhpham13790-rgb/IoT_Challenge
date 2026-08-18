/* ============================================================================
 *  oled_display.c — bedside 128x64 I2C OLED (Smart IV, ICTU team)
 *
 *  Panel handling (init sequence, SH1106 column offset, page refresh, the 5x7
 *  glyph idea) comes from the heart-rate bench build in IoT_Challenge-main
 *  (2026-08-11), where it was proven on the real 1.3" module. The screen
 *  CONTENT is this firmware's, and the font table was completed to a full
 *  A-Z so new wording doesn't silently render as blanks — the bench build
 *  carried only the 17 letters its two screens happened to use.
 * ========================================================================== */
#include "oled_display.h"

#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Panel transport
 * ------------------------------------------------------------------------- */

static bool send_commands(oled_display_t *display,
                          const uint8_t *commands, uint8_t length)
{
  return display->bus.write(display->bus.context, display->address,
                            0x00U, commands, length);
}

static bool refresh(oled_display_t *display)
{
  /* SH1106 has 132 RAM columns and shows column 0 of a 128-wide image at RAM
   * column 2, hence the 0x02/0x10 low/high column address. An SSD1306 (132
   * columns' worth of commands, 128 visible) accepts the same sequence, which
   * is why one code path covers both controllers sold as "1.3 inch OLED". */
  for (uint8_t page = 0U; page < 8U; page++) {
    uint8_t commands[] = { (uint8_t)(0xB0U + page), 0x02U, 0x10U };
    if (!send_commands(display, commands, sizeof(commands))
        || !display->bus.write(display->bus.context, display->address,
                               0x40U,
                               &display->framebuffer[(uint16_t)page * OLED_WIDTH],
                               OLED_WIDTH)) {
      return false;
    }
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * 5x7 font, column-major, bit 0 = top pixel
 * ------------------------------------------------------------------------- */

static const uint8_t FONT_DIGITS[10][5] = {
  {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
  {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}
};

static const uint8_t FONT_LETTERS[26][5] = {
  {0x7E,0x11,0x11,0x11,0x7E}, /* A */ {0x7F,0x49,0x49,0x49,0x36}, /* B */
  {0x3E,0x41,0x41,0x41,0x22}, /* C */ {0x7F,0x41,0x41,0x22,0x1C}, /* D */
  {0x7F,0x49,0x49,0x49,0x41}, /* E */ {0x7F,0x09,0x09,0x09,0x01}, /* F */
  {0x3E,0x41,0x49,0x49,0x7A}, /* G */ {0x7F,0x08,0x08,0x08,0x7F}, /* H */
  {0x00,0x41,0x7F,0x41,0x00}, /* I */ {0x20,0x40,0x41,0x3F,0x01}, /* J */
  {0x7F,0x08,0x14,0x22,0x41}, /* K */ {0x7F,0x40,0x40,0x40,0x40}, /* L */
  {0x7F,0x02,0x0C,0x02,0x7F}, /* M */ {0x7F,0x04,0x08,0x10,0x7F}, /* N */
  {0x3E,0x41,0x41,0x41,0x3E}, /* O */ {0x7F,0x09,0x09,0x09,0x06}, /* P */
  {0x3E,0x41,0x51,0x21,0x5E}, /* Q */ {0x7F,0x09,0x19,0x29,0x46}, /* R */
  {0x46,0x49,0x49,0x49,0x31}, /* S */ {0x01,0x01,0x7F,0x01,0x01}, /* T */
  {0x3F,0x40,0x40,0x40,0x3F}, /* U */ {0x1F,0x20,0x40,0x20,0x1F}, /* V */
  {0x3F,0x40,0x38,0x40,0x3F}, /* W */ {0x63,0x14,0x08,0x14,0x63}, /* X */
  {0x07,0x08,0x70,0x08,0x07}, /* Y */ {0x61,0x51,0x49,0x45,0x43}  /* Z */
};

static void glyph(char c, uint8_t out[5])
{
  if (c >= '0' && c <= '9') {
    memcpy(out, FONT_DIGITS[(unsigned)(c - '0')], 5U);
    return;
  }
  if (c >= 'A' && c <= 'Z') {
    memcpy(out, FONT_LETTERS[(unsigned)(c - 'A')], 5U);
    return;
  }

  static const uint8_t PERCENT[5] = {0x23,0x13,0x08,0x64,0x62};
  static const uint8_t DASH[5]    = {0x08,0x08,0x08,0x08,0x08};
  static const uint8_t COLON[5]   = {0x00,0x36,0x36,0x00,0x00};
  static const uint8_t DOT[5]     = {0x00,0x60,0x60,0x00,0x00};
  static const uint8_t SLASH[5]   = {0x20,0x10,0x08,0x04,0x02};

  const uint8_t *p = NULL;
  switch (c) {
    case '%': p = PERCENT; break;
    case '-': p = DASH;    break;
    case ':': p = COLON;   break;
    case '.': p = DOT;     break;
    case '/': p = SLASH;   break;
    default:  break;                /* space, and anything unmapped, is blank */
  }
  if (p != NULL) { memcpy(out, p, 5U); } else { memset(out, 0, 5U); }
}

/* ---------------------------------------------------------------------------
 * Framebuffer drawing
 * ------------------------------------------------------------------------- */

static void pixel(oled_display_t *display, uint8_t x, uint8_t y)
{
  if (x < OLED_WIDTH && y < OLED_HEIGHT) {
    display->framebuffer[(uint16_t)x + ((uint16_t)y / 8U) * OLED_WIDTH]
      |= (uint8_t)(1U << (y & 7U));
  }
}

static void draw_char(oled_display_t *display, uint8_t x, uint8_t y,
                      char c, uint8_t scale)
{
  uint8_t columns[5];
  glyph(c, columns);
  for (uint8_t col = 0U; col < 5U; col++) {
    for (uint8_t row = 0U; row < 7U; row++) {
      if ((columns[col] & (1U << row)) != 0U) {
        for (uint8_t dx = 0U; dx < scale; dx++) {
          for (uint8_t dy = 0U; dy < scale; dy++) {
            pixel(display, (uint8_t)(x + col * scale + dx),
                  (uint8_t)(y + row * scale + dy));
          }
        }
      }
    }
  }
}

static void draw_text(oled_display_t *display, uint8_t x, uint8_t y,
                      const char *text, uint8_t scale)
{
  while (*text != '\0') {
    draw_char(display, x, y, *text++, scale);
    x = (uint8_t)(x + 6U * scale);
  }
}

static uint8_t text_width(const char *text, uint8_t scale)
{
  uint8_t chars = 0U;
  while (text[chars] != '\0') chars++;
  /* The trailing inter-character gap isn't part of the visible glyph run. */
  return chars == 0U ? 0U : (uint8_t)(chars * 6U * scale - scale);
}

static void draw_text_centered(oled_display_t *display, uint8_t y,
                               const char *text, uint8_t scale)
{
  uint8_t width = text_width(text, scale);
  uint8_t x = width >= OLED_WIDTH ? 0U : (uint8_t)((OLED_WIDTH - width) / 2U);
  draw_text(display, x, y, text, scale);
}

static void draw_frame(oled_display_t *display, uint8_t x, uint8_t y,
                       uint8_t width, uint8_t height)
{
  for (uint8_t i = 0U; i < width; i++) {
    pixel(display, (uint8_t)(x + i), y);
    pixel(display, (uint8_t)(x + i), (uint8_t)(y + height - 1U));
  }
  for (uint8_t i = 0U; i < height; i++) {
    pixel(display, x, (uint8_t)(y + i));
    pixel(display, (uint8_t)(x + width - 1U), (uint8_t)(y + i));
  }
}

static void clear(oled_display_t *display)
{
  memset(display->framebuffer, 0, sizeof(display->framebuffer));
}

/* Renders a reading, or "--" when the channel has no signal. A channel that
 * isn't measuring must never show a number: an unplugged probe reads 0, and
 * "0" on a screen at a bedside is a reading, not an absence of one. */
static void format_reading(char out[4], bool valid, uint16_t value)
{
  if (!valid) { out[0] = '-'; out[1] = '-'; out[2] = '\0'; return; }
  if (value > 999U) value = 999U;

  uint8_t i = 0U;
  if (value >= 100U) out[i++] = (char)('0' + value / 100U);
  if (value >= 10U)  out[i++] = (char)('0' + (value / 10U) % 10U);
  out[i++] = (char)('0' + value % 10U);
  out[i] = '\0';
}

/* ---------------------------------------------------------------------------
 * Screens
 * ------------------------------------------------------------------------- */

bool oled_display_show_splash(oled_display_t *display)
{
  if (display == NULL || !display->initialized) return false;
  clear(display);
  draw_text_centered(display, 12U, "SMART IV", 2U);
  draw_text_centered(display, 34U, "ICTU", 2U);
  draw_text_centered(display, 54U, "DANG KHOI DONG", 1U);
  return refresh(display);
}

/* The single alarm line. Ordered by severity to match DescribeAlert() in the
 * HIS Server (VitalsStatusEvaluator.cs), so the bed and the ward console name
 * the same problem first — a nurse walking from the console to the bed must
 * not be told two different stories. */
static const char *alarm_banner(const oled_vitals_t *v)
{
  if (v->reason_spo2)    return "SPO2 THAP";
  if (v->reason_flow)    return "TAC DAY TRUYEN";
  if (v->reason_hr)      return "NHIP TIM BAT THUONG";
  if (v->reason_missing) return "MAT TIN HIEU CAM BIEN";
  if (v->reason_ai)      return "AI BAO BAT THUONG";
  return "CANH BAO";
}

bool oled_display_show_vitals(oled_display_t *display,
                              const oled_vitals_t *vitals)
{
  if (display == NULL || !display->initialized || vitals == NULL) return false;

  char hr_text[4];
  char spo2_text[4];
  char flow_text[8];
  format_reading(hr_text, vitals->hr_valid, vitals->hr_bpm);
  format_reading(spo2_text, vitals->spo2_valid, vitals->spo2_pct);

  /* Flow is a ratio against the doctor's target, and unlike HR/SpO2 it can be
   * far above 100% (free flow), so it gets its own formatting with a cap. */
  if (!vitals->flow_valid) {
    memcpy(flow_text, "--", 3U);
  } else {
    uint16_t pct = vitals->flow_pct < 0 ? 0U : (uint16_t)vitals->flow_pct;
    char digits[4];
    format_reading(digits, true, pct);
    uint8_t i = 0U;
    while (digits[i] != '\0') { flow_text[i] = digits[i]; i++; }
    flow_text[i++] = '%';
    flow_text[i] = '\0';
  }

  const char *banner = vitals->alarm ? alarm_banner(vitals) : "BINH THUONG";

  /* Nothing changed since the last paint -> send nothing. A full frame is
   * 8 pages x 128 bytes; pushing that down a bit-banged bus every second
   * would burn ~100ms of the 1s AI cycle for an identical picture. */
  static char last_signature[48];
  char signature[48];
  int len = 0;
  const char *parts[4] = { hr_text, spo2_text, flow_text, banner };
  for (uint8_t p = 0U; p < 4U && len < (int)sizeof(signature) - 2; p++) {
    for (uint8_t i = 0U; parts[p][i] != '\0' && len < (int)sizeof(signature) - 2; i++) {
      signature[len++] = parts[p][i];
    }
    signature[len++] = '|';
  }
  signature[len] = '\0';
  if (strcmp(signature, last_signature) == 0) return true;

  clear(display);

  /* Top half: the two numbers a nurse reads from across the bed, as large as
   * 128x64 allows (scale 3 = 15x21 px per glyph). */
  draw_text(display, 2U,  0U, "HR", 1U);
  draw_text(display, 70U, 0U, "SPO2", 1U);
  draw_text(display, 2U,  10U, hr_text, 3U);
  draw_text(display, 70U, 10U, spo2_text, 3U);

  /* Middle: flow against the prescription. Small on purpose — it matters when
   * setting the drip, not when glancing from the doorway. */
  draw_text(display, 2U, 34U, "FLOW", 1U);
  draw_text(display, 34U, 34U, flow_text, 1U);
  draw_text(display, 78U, 34U, "MUC DAT", 1U);

  /* Bottom: one line saying whether anything is wrong, boxed when it is. The
   * box is what makes an alarm visible in peripheral vision; without it a
   * nurse has to actually read the line to know it changed. */
  if (vitals->alarm) {
    draw_frame(display, 0U, 44U, OLED_WIDTH, 20U);
    draw_text_centered(display, 52U, banner, 1U);
  } else {
    draw_text_centered(display, 52U, banner, 1U);
  }

  if (!refresh(display)) {
    /* Leave the signature stale so the next call retries this same frame
     * instead of assuming the panel already shows it. */
    last_signature[0] = '\0';
    return false;
  }
  memcpy(last_signature, signature, (size_t)len + 1U);
  return true;
}

/* ---------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------- */

static bool init_at_address(oled_display_t *display, const oled_bus_t *bus,
                            uint8_t address)
{
  memset(display, 0, sizeof(*display));
  display->bus = *bus;
  display->address = address;

  static const uint8_t init[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
    /* 0x8D/0x14 turns on the SSD1306 charge pump; 0xAD/0x8B the equivalent
     * SH1106 DC-DC converter. Each controller ignores the other's command, so
     * sending both covers either module without knowing which one is fitted —
     * and without one of them the panel initialises but stays dark. */
    0x8D, 0x14, 0xAD, 0x8B, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF,
    0xD9, 0x22, 0xDB, 0x35, 0xA4, 0xA6, 0xAF
  };
  if (!send_commands(display, init, sizeof(init))) return false;
  display->initialized = true;
  return true;
}

bool oled_display_init(oled_display_t *display, const oled_bus_t *bus)
{
  if (display == NULL || bus == NULL || bus->write == NULL) return false;

  if (!init_at_address(display, bus, OLED_I2C_ADDRESS)
      && !init_at_address(display, bus, OLED_I2C_ADDRESS_ALT)) {
    display->initialized = false;
    return false;
  }
  return oled_display_show_splash(display);
}
