/* ============================================================================
 *  clinical_limits.h — the hard rules, in one place
 *
 *  These are the limits the device alarms on WITHOUT any model being involved.
 *  They exist as a separate header, rather than inside whichever AI file
 *  happens to use them, for one reason: they must survive the AI. If no model
 *  loads, if every interpreter fails, if the flash holding the models is
 *  corrupt - these still run, and the device is still a monitor.
 *
 *  Two properties are deliberate and must not be "tidied up":
 *
 *  1. NONE of these go through the K=11 persistence filter. Persistence is
 *     justified for AI-detected anomalies, where a two-second blip is usually a
 *     cough or a patient shifting position. It is not justified for SpO2 below
 *     90% - that needs someone at the bedside now, not in eleven seconds.
 *
 *  2. The heart-rate rule is relative to THIS PATIENT'S baseline, captured over
 *     the first ~60 s after the sensor is attached, with absolute floor and
 *     ceiling on top. A resting rate of 55 and one of 95 are both normal, for
 *     different people; a fixed band would either miss deterioration in the
 *     first patient or cry wolf about the second.
 * ========================================================================== */
#ifndef CLINICAL_LIMITS_H
#define CLINICAL_LIMITS_H

/* Heart rate deviating more than this fraction from the patient's own
 * baseline, while still inside the absolute bounds below. */
#define AI_HR_PCT       0.30f

/* Absolute bounds. Past these it is severe brady- or tachycardia regardless of
 * what the patient's baseline was. */
#define AI_HR_ABS_LOW   45.0f
#define AI_HR_ABS_HIGH  150.0f

/* Oxygen saturation. 90% is the conventional desaturation threshold. */
#define AI_SPO2_ABS     90.0f

/* Flow and drop rate as a fraction of what the doctor prescribed.
 * Above HI: free flow (the roller clamp has slipped, or the bag is squeezed).
 * Below LO: occluded, kinked, or the vein has blown. */
#define AI_FLOW_HI      1.5f
#define AI_FLOW_LO      0.3f

#endif /* CLINICAL_LIMITS_H */
