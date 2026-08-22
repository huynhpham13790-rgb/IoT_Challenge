/* ============================================================================
 *  sensor_hub.h — Manages sensor channels for the AI module (Smart IV, ICTU team)
 *  Idea: each channel has 3 STATES
 *    CH_DISABLED : sensor not connected -> skip, do NOT raise false alarms
 *    CH_OK       : fresh data available -> read real values
 *    CH_LOST     : connected but signal lost -> set missing flag -> ALARM
 *  Enable/disable each channel with the #define below.
 * ========================================================================== */
#ifndef SENSOR_HUB_H
#define SENSOR_HUB_H

#include <stdint.h>
#include <stdbool.h>

/* ===== SWITCHES: 0 = not connected (skip) | 1 = connected (read real data) =====
 * All 4 channels are physically connected (confirmed by the user on
 * 2026-07-18). To disconnect a channel, set its #define back to 0 to avoid
 * reading noise on a floating pin. */
#define HR_ENABLED     1   // MAX30102 - heart rate (I2C, shares bus with SpO2) - CONNECTED
#define SPO2_ENABLED   1   // MAX30102 - SpO2 (same chip as HR) - CONNECTED
#define FLOW_ENABLED   1   // HX711 + load cell - infusion flow rate - CONNECTED
#define DROPS_ENABLED  1   // drop sensor - CONNECTED

/* ===== Doctor-set targets — adjust per prescription =====
 * Both of these are only STARTUP DEFAULTS - the doctor can change the live
 * value at runtime (from the HIS Server, via the writable TargetFlowMlH /
 * TargetDropsPerMin Zigbee attributes) using sh_set_target_flow_ml_h() /
 * sh_set_target_drops_per_min(); sh_flow_ratio()/sh_drops_ratio() and their
 * matching getters always reflect whatever was last set, not these
 * compile-time constants. */
#define SET_FLOW_ML_H   100.0f   // default target flow rate (ml/hour)
#define SET_DROPS_DPM   20.0f    // default target drops per minute

/* ===== Load cell (HX711) calibration — ADJUST for the actual scale in use =====
 * Calibration method: hang a weight of known mass, compare the raw reading
 * against the no-load reading, then compute factor = delta_raw / mass_KG
 * (matches the weight_g = delta_raw / FACTOR * 1000 formula used in
 * sensor_hub.c). Calibrated for real hardware on 2026-07-17 (2nd attempt,
 * measured ACCURATELY within a SINGLE tare+load session to avoid drift
 * between reflashes): hung a ~500ml (~500g water) IV bag right at its
 * printed graduation mark, tare_offset=-39289, delta_raw after settling
 * (EMA) ~101267 -> factor = 101267/0.5kg = 202534. Older values (14000 from
 * hx711_test.ino, 269334 from an earlier measurement that mixed tare/load
 * from two different flash sessions) are both WRONG and no longer used. */
#define HX711_CALIBRATION_FACTOR   202534.0f

/* ===== Time before a channel is considered "signal lost" if enabled but no sample arrives (ms) ===== */
#define VITAL_TIMEOUT_MS  3000U

typedef enum { CH_DISABLED = 0, CH_OK = 1, CH_LOST = 2 } ch_state_t;

/* ===== Local alarm indicators: 3 LEDs + buzzer =====
 * Wiring on BRD2709A (mikroBUS header), confirmed by the user on 2026-07-25:
 *   GREEN  LED -> PWM pin, PA07
 *   YELLOW LED -> TX  pin, PA04   (mikroBUS UART, NOT the VCOM console,
 *   RED    LED -> RX  pin, PA05    which is on PB02/PB03 - no conflict)
 *   BUZZER     -> PC06
 * Each LED needs a 220-330 ohm series resistor; drive the buzzer through an
 * NPN transistor if it draws more than a few mA (GPIO limit).
 * All four outputs are ACTIVE HIGH (set ALERT_ACTIVE_HIGH to 0 in
 * sensor_hub.c if your modules are the active-low kind).
 *
 * Còi đã đi qua CS/PC04 -> MOSI/PC02 -> CS/PC04 -> PC06. Lần nó im tiếng, nguyên
 * nhân hoá ra là NGUỒN YẾU chứ không phải sai chân — nên đừng đổi chân để chữa
 * một cái còi không kêu; đo nguồn trước. Ba chân kia giờ đều trống.
 *
 * Dù vậy, hai điều rút ra vẫn còn giá trị:
 *
 *  1. Nếu code trỏ nhầm chân thì còi im HOÀN TOÀN mà không lỗi nào hiện ra —
 *     chip đẩy tín hiệu ra một chân trống, chân có còi thì không ai đụng tới.
 *     Vì vậy sensor_hub.c có _Static_assert chặn hai thiết bị trùng chân.
 *
 *  2. Theo SƠ ĐỒ CHÂN CỦA BOARD: MOSI = PC02, MISO = PC01 (chỗ cắm dây dữ
 *     liệu của cân). Điều này NGƯỢC với cách suy từ
 *     sl_spidrv_usart_mikroe_config.h ("SPI master thì TX = MOSI", file đó ghi
 *     TX = PC01). SDK chỉ nói TX/RX chứ không nói MOSI/MISO — đã suy sai một
 *     lần theo hướng đó, nên tra sơ đồ chân, đừng suy luận. */
/* Four levels, because the two RED conditions need different responses from
 * the nurse and used to be indistinguishable:
 *
 *   LINE_WARNING  the infusion line is in trouble, the PATIENT is fine.
 *                 Check the tubing. Not an emergency.
 *   VITALS_ALERT  the patient is in trouble. Go to the bedside.
 *   CRITICAL      BOTH at once - fluid pouring in while the patient
 *                 deteriorates. Suspected fluid overload / anaphylaxis.
 *
 * Only three LEDs exist, so CRITICAL is shown as RED + YELLOW together with a
 * distinctly faster beep. A nurse across the ward reads urgency from the beep
 * rate; the exact reason travels to the HIS Server in the alarm bitmap. */
typedef enum {
  ALERT_LEVEL_NORMAL       = 0,
  ALERT_LEVEL_LINE_WARNING = 1,
  ALERT_LEVEL_VITALS_ALERT = 2,
  ALERT_LEVEL_CRITICAL     = 3
} alert_level_t;

/* Sets the level to indicate. Cheap and idempotent - call it as often as you
 * like (app.c calls it once per AI tick). The actual LED/buzzer driving is
 * done by sh_alert_poll(), which sensor_hub_poll() already calls. */
void          sh_alert_set_level(alert_level_t level);
alert_level_t sh_alert_level(void);

/* Human-readable name, for the serial log and the OLED. */
const char   *sh_alert_level_name(alert_level_t level);

void sensor_hub_init(void);
void sensor_hub_poll(void);   // call EVERY loop iteration (reads drops continuously)

/* ===== Shared I2C bus (PC05=SCL, PC07=SDA) =====
 * sensor_hub owns the bit-banged bus for the MAX30102; the bedside OLED sits
 * on the same two wires at a different address and writes through here rather
 * than bit-banging the pins itself. One owner means two devices can never
 * interleave half-finished transactions on the bus.
 *
 * `first_byte` is whatever goes right after the address: the register number
 * for a sensor, or the OLED's control byte (0x00 = command, 0x40 = display
 * RAM). Returns false if the device did not ACK. */
bool sh_i2c_write(uint8_t address, uint8_t first_byte,
                  const uint8_t *data, uint8_t len);

/* Current raw value of each channel (only meaningful when state = CH_OK) */
float      sh_hr(void);
float      sh_spo2(void);
float      sh_flow_ratio(void);    // flow / target_flow_ml_h
float      sh_flow_weight_g(void); // raw scale reading, grams
float      sh_drops_ratio(void);   // drops_per_min / target_drops_per_min
float      sh_drops_per_min(void);

/* Per-channel state */
ch_state_t sh_hr_state(void);
ch_state_t sh_spo2_state(void);
ch_state_t sh_flow_state(void);
ch_state_t sh_drops_state(void);

uint32_t   sh_total_drops(void);

/* Doctor-set target infusion rate, ml/hour - readable/settable at runtime
 * (see sensor_hub.c for how a network Zigbee attribute write reaches this). */
float      sh_target_flow_ml_h(void);

/* ===== Chế độ chờ / đang theo dõi =====
 *
 * Ở chế độ CHỜ, thiết bị vẫn đọc cảm biến và vẫn hiện số lên màn hình - y tá
 * cần thấy kẹp SpO2 đã ăn tín hiệu chưa, cân đã về 0 chưa - nhưng KHÔNG chạy
 * AI, KHÔNG báo động và KHÔNG kêu còi.
 *
 * Đó là giai đoạn treo bình, kẹp cảm biến, tare cân: mọi số đo đều là rác và
 * mọi tiếng còi đều là báo giả. Bắt một cái máy y tế phải im trong lúc chưa ai
 * bảo nó bắt đầu là cách duy nhất giữ cho tiếng còi của nó còn nghĩa lý.
 *
 * sh_set_monitoring_active() trả về true khi trạng thái thật sự đổi. */
bool       sh_monitoring_active(void);
bool       sh_set_monitoring_active(bool active);
void       sh_set_target_flow_ml_h(float ml_per_h);

/* Doctor-set target drop rate, drops/min - same idea as target_flow_ml_h. */
float      sh_target_drops_per_min(void);
void       sh_set_target_drops_per_min(float dpm);

/* Loadcell tare-button status (BTN0, or triggered remotely - see
 * sh_flow_trigger_tare()) - see sensor_hub.c for details. */
bool       sh_flow_tare_in_progress(void);
bool       sh_flow_tare_just_completed(void);   // one-shot, consumes itself
uint8_t    sh_flow_tare_event_count(void);       // persistent, never resets - see sensor_hub.c

/* Re-arms the tare state machine - the same thing BTN0 does, but callable
 * directly (e.g. from app.c's post_attribute_change_cb when the doctor
 * triggers "reset scale" remotely from the HIS Server). */
void       sh_flow_trigger_tare(void);

#endif /* SENSOR_HUB_H */
