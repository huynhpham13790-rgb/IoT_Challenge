/* ============================================================================
 *  line_rules.h — telling "the bag is nearly empty" apart from "the line is
 *                 blocked", using the load cell instead of a neural network
 *
 *  --- Why this is arithmetic and not a model ------------------------------
 *
 *  The original specification made bag weight the second input channel of the
 *  drip forecaster. That was dropped, for three reasons:
 *
 *   1. There is not one byte of real load-cell training data anywhere in this
 *      project. The channel would have been entirely simulated, so the network
 *      would have learned the relationship the data generator was written to
 *      produce - a fabricated correlation, which is the exact defect the whole
 *      v2 redesign exists to remove.
 *   2. Weight and drops measure the SAME physical event, fluid leaving the bag:
 *      dW/dt ~= drops_per_minute * millilitres_per_drop. A second channel that
 *      is a linear function of the first adds almost no information.
 *   3. The relationship is simple, deterministic, and known in advance. Making
 *      a network rediscover a rule we can write down costs flash, costs RAM,
 *      and loses the ability to explain the result to a nurse.
 *
 *  --- The physical insight this encodes -----------------------------------
 *
 *  A full bag has a tall column of fluid above the drip chamber, so hydrostatic
 *  pressure is high and flow is fast. As it empties the column shortens, the
 *  pressure falls, and flow slows. SLOWING FLOW NEAR THE END OF A BAG IS
 *  NORMAL. Alarming on it trains nurses to ignore the device.
 *
 *  Drop rate alone cannot tell that apart from a forming occlusion - both look
 *  like "fewer drops per minute". The load cell settles it outright:
 *
 *    weight falling  + drops slowing  -> fluid IS leaving. Emptying bag or low
 *                                        head. Normal; just say "running low".
 *    weight STEADY   + drops slowing  -> fluid is NOT leaving. OCCLUSION.
 *    weight dropping fast + drops up  -> FREE FLOW.
 *    weight steady   + drops normal   -> the drop sensor is counting something
 *                                        that is not fluid (sunlight, vibration)
 *                                        -> a TECHNICAL fault, not a clinical one.
 *
 *  The last row matters as much as the others: reporting a sensor fault as a
 *  patient alarm sends a nurse to the wrong problem.
 * ========================================================================== */
#ifndef LINE_RULES_H
#define LINE_RULES_H

#include <stdbool.h>
#include <stdint.h>

/* Drops per millilitre for the giving set in use. 20 gtt/mL is the standard
 * macro-drip adult set; paediatric micro-drip sets are 60. It is printed on the
 * tubing package. Wrong value here does not break occlusion detection (that
 * only needs "is weight moving at all") but does skew the remaining-time
 * estimate proportionally. */
#ifndef LINE_DROP_FACTOR_GTT_PER_ML
#define LINE_DROP_FACTOR_GTT_PER_ML  20.0f
#endif

/* How long a trend is measured over. Long enough that load-cell noise averages
 * out, short enough to notice an occlusion promptly. At 1 Hz this is one
 * sample per second. */
#define LINE_TREND_WINDOW_S   60

/* "Steady" means the bag is not losing weight anywhere near as fast as the
 * counted drops say it should be.
 *
 * This was originally an absolute constant, 3 g over the 60 s window, and the
 * host test caught why that is wrong: a perfectly normal adult infusion at 60
 * drops/min with a 20 gtt/mL set removes 3 mL - 3 g - per minute. The threshold
 * for "not moving" was exactly the normal rate, so an entirely healthy infusion
 * was reported as a drop-sensor fault.
 *
 * The comparison has to scale with the prescribed rate, because that is what
 * the cross-check is actually about: does the weight agree with the drops. A
 * fixed number cannot be right for both a 15 dpm keep-vein-open drip and a 120
 * dpm bolus.
 *
 * LINE_STEADY_FRACTION: below this fraction of the rate the drops imply, the
 * weight is not keeping up and the two sensors disagree.
 * LINE_NOISE_G_MIN: absolute floor, so that when the drops stop entirely (and
 * the implied rate is zero) the test falls back to the load cell's own noise
 * rather than to zero, which nothing real ever reads. */
#define LINE_STEADY_FRACTION  0.40f
#define LINE_NOISE_G_MIN      0.5f    /* grams per minute */

/* Bag considered nearly empty below this many grams above the tare point. */
#define LINE_NEARLY_EMPTY_G  50.0f

typedef enum {
  LINE_OK = 0,          /* flow and weight agree, nothing to say */
  LINE_RUNNING_LOW,     /* emptying normally, tell the nurse the time left */
  LINE_OCCLUSION,       /* drops slowed but weight is not moving */
  LINE_FREE_FLOW,       /* emptying far faster than prescribed */
  LINE_SENSOR_MISMATCH, /* drops counted while weight does not move */
  LINE_EMPTY            /* bag is done */
} line_state_t;

typedef struct {
  line_state_t state;
  bool  valid;             /* false when the load cell is absent or still
                            * filling its trend window - callers must not
                            * treat anything below as meaningful */

  float weight_g;          /* current reading */
  float weight_rate_g_min; /* negative = emptying */
  float expected_rate_g_min; /* what the drop rate implies it should be */

  float remaining_ml;      /* estimate, from weight above the empty point */
  int32_t remaining_min;   /* at the CURRENT rate; -1 when not estimable */

  bool  weight_steady;     /* losing weight far slower than the drops imply */
  bool  drops_slow;        /* drop ratio below its low band */
  bool  drops_fast;        /* drop ratio above its high band */
} line_result_t;

/* Call once at start-up. */
void line_rules_init(void);

/* Call once per second, after sensor_hub_poll(). Fills `out`. */
void line_rules_step(line_result_t *out);

/* Short text for the OLED and the serial log. */
const char *line_state_text(line_state_t s);

#endif /* LINE_RULES_H */
