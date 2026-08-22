/* ============================================================================
 *  line_rules.c — see line_rules.h for why this is arithmetic, not a model
 * ========================================================================== */

#include "line_rules.h"
#include "sensor_hub.h"
#include "clinical_limits.h"

#include <math.h>
#include <string.h>

/* Ring buffer of weight readings, one per second. Only the oldest and newest
 * are needed for a trend, but keeping the whole window means a single dropout
 * cannot corrupt the estimate the way a two-sample difference would. */
static float    s_weight[LINE_TREND_WINDOW_S];
static uint8_t  s_count;   /* how many valid samples so far */
static uint8_t  s_head;

void line_rules_init(void)
{
  memset(s_weight, 0, sizeof(s_weight));
  s_count = 0;
  s_head  = 0;
}

const char *line_state_text(line_state_t s)
{
  switch (s) {
    case LINE_RUNNING_LOW:     return "Bag running low";
    case LINE_OCCLUSION:       return "LINE BLOCKED";
    case LINE_FREE_FLOW:       return "FREE FLOW";
    case LINE_SENSOR_MISMATCH: return "Drop sensor fault";
    case LINE_EMPTY:           return "BAG EMPTY";
    default:                   return "Line OK";
  }
}

void line_rules_step(line_result_t *out)
{
  memset(out, 0, sizeof(*out));
  out->state = LINE_OK;
  out->remaining_min = -1;

  const bool have_weight = (sh_flow_state() == CH_OK);
  const bool have_drops  = (sh_drops_state() == CH_OK);

  if (!have_weight) {
    /* Without the load cell none of this can be decided. Say so rather than
     * guessing - the drip forecaster still runs, and the flow/drop ratio rules
     * still apply; they just cannot separate "empty" from "blocked". */
    out->valid = false;
    return;
  }

  const float w = sh_flow_weight_g();
  out->weight_g = w;

  s_weight[s_head] = w;
  s_head = (uint8_t)((s_head + 1) % LINE_TREND_WINDOW_S);
  if (s_count < LINE_TREND_WINDOW_S) {
    s_count++;
  }

  /* A trend needs the full window. Reporting one from four samples would make
   * the occlusion test fire on load-cell noise during the first minute after
   * power-up, which is exactly when a nurse is still hanging the bag. */
  if (s_count < LINE_TREND_WINDOW_S) {
    out->valid = false;
    return;
  }
  out->valid = true;

  const uint8_t oldest_idx = s_head;      /* head now points at the oldest */
  const float   oldest     = s_weight[oldest_idx];
  const float   change_g   = w - oldest;  /* negative while emptying */

  out->weight_rate_g_min = change_g * (60.0f / (float)LINE_TREND_WINDOW_S);

  /* What the counted drops imply the weight SHOULD be doing. 1 mL of aqueous
   * infusate weighs ~1 g, close enough at this resolution. */
  const float dpm = have_drops ? sh_drops_per_min() : 0.0f;
  out->expected_rate_g_min = -(dpm / LINE_DROP_FACTOR_GTT_PER_ML);

  /* Steady = losing weight far more slowly than the drops claim. Relative to
   * the implied rate, not an absolute constant - see LINE_STEADY_FRACTION in
   * the header for the bug that taught us this. */
  const float implied = fabsf(out->expected_rate_g_min);
  float steady_limit = LINE_STEADY_FRACTION * implied;
  if (steady_limit < LINE_NOISE_G_MIN) {
    steady_limit = LINE_NOISE_G_MIN;
  }
  out->weight_steady = (fabsf(out->weight_rate_g_min) < steady_limit);

  const float ratio = have_drops ? sh_drops_ratio() : 1.0f;
  out->drops_slow = have_drops && (ratio < AI_FLOW_LO);
  out->drops_fast = have_drops && (ratio > AI_FLOW_HI);

  /* Remaining volume and time. Both are estimates and both are labelled as
   * such upstream; the value to a nurse is "about twenty minutes", not a
   * number to the millilitre. */
  out->remaining_ml = (w > 0.0f) ? w : 0.0f;
  if (out->weight_rate_g_min < -0.5f) {
    out->remaining_min = (int32_t)(out->remaining_ml / -out->weight_rate_g_min);
  }

  /* ---- The decision table from line_rules.h, in priority order ------------
   * Order matters: an empty bag also looks "steady", and a blocked line also
   * looks "slow", so the more specific condition has to be tested first. */

  if (w <= LINE_NEARLY_EMPTY_G && out->weight_steady) {
    /* Nothing left and nothing moving - the infusion is over. */
    out->state = LINE_EMPTY;
    return;
  }

  if (out->drops_slow && out->weight_steady) {
    /* Drops have slowed AND fluid is not leaving the bag. It is not going
     * anywhere else, so the line is blocked. This is the case a drop sensor
     * alone can never distinguish from a bag simply running out. */
    out->state = LINE_OCCLUSION;
    return;
  }

  if (out->drops_slow && !out->weight_steady) {
    /* Drops slowed but fluid IS still leaving: the head of fluid has fallen,
     * which is what every bag does as it empties. Normal - flag it only so the
     * nurse knows to plan a change. */
    out->state = (w <= LINE_NEARLY_EMPTY_G * 3.0f) ? LINE_RUNNING_LOW : LINE_OK;
    return;
  }

  if (out->drops_fast && out->weight_rate_g_min < 2.0f * out->expected_rate_g_min) {
    /* Counting fast AND losing weight faster than prescribed - both sensors
     * agree the bag is emptying too quickly. */
    out->state = LINE_FREE_FLOW;
    return;
  }

  if (have_drops && dpm > 1.0f && out->weight_steady) {
    /* Drops are being counted at a normal rate while the bag does not get any
     * lighter. Fluid cannot leave the bag without the bag getting lighter, so
     * the drop sensor is counting something that is not a drop - sunlight on
     * the photodiode, or vibration. A technical fault, and reporting it as a
     * clinical alarm would send someone to the wrong problem. */
    out->state = LINE_SENSOR_MISMATCH;
    return;
  }

  if (w <= LINE_NEARLY_EMPTY_G * 3.0f) {
    out->state = LINE_RUNNING_LOW;
  }
}
