#!/usr/bin/env python3
"""
Generate a 1 Hz TIME-SERIES dataset for Smart IV.

The key difference from the older `iv_vitals_synthetic_labeled.csv`: that one
labelled EACH ROW independently, so it cannot train or evaluate a model that has
a time axis. This generator produces PROCESSES over time, and separates the two
kinds of abnormality that the model must treat DIFFERENTLY:

  - TRANSIENT (a 2-6 second blip): the patient shifting position, brushing the
    PPG sensor, one drop falling off-centre. -> label_alarm = 0 (must NOT alarm)
  - SUSTAINED (>= 45 seconds): occlusion, free flow, desaturation, tachy- or
    bradycardia. -> label_alarm = 1 (MUST alarm)

Without the first category there is nothing against which to MEASURE a reduction
in false alarms - precisely the gap that made the old model unable to answer the
supervisor's criticism.

Every sustained event has a GRADUAL ONSET (ramp) rather than a step change. That
is what makes EARLY warning possible for a forecasting model: if everything
changed instantaneously there would be nothing left to forecast.

Signals are generated with an AR(1) autoregressive process plus a slow
oscillation, rather than drawing an independent sample every second, so the
series has the temporal correlation of real physiology.
"""

import argparse
import numpy as np
import pandas as pd

FS_HZ = 1.0          # 1 sample/second - matches the chip AI cycle (AI_PERIOD_MS = 1000)
CHANNELS = ["heart_rate", "spo2", "drops_per_min", "weight_g"]

# Normal ranges (calibrated against the adult ICU patient distribution)
HR_BASE_RANGE = (62.0, 94.0)
SPO2_BASE_RANGE = (96.0, 99.0)


def ar1(n, rho, sigma, rng):
    """Zero-mean AR(1) process: x[t] = rho*x[t-1] + noise.

    Produces the physiological wander that has temporal correlation. Independent
    sampling (rho = 0) would give white noise, from which the model could learn
    nothing about dynamics.
    """
    x = np.zeros(n, dtype=np.float64)
    for t in range(1, n):
        x[t] = rho * x[t - 1] + rng.normal(0.0, sigma)
    return x


def make_patient(rng, minutes, target_dpm, target_flow_ml_h):
    """Generate one completely normal patient run as the baseline."""
    n = int(minutes * 60 * FS_HZ)

    hr_base = rng.uniform(*HR_BASE_RANGE)
    spo2_base = rng.uniform(*SPO2_BASE_RANGE)

    # HR: AR(1) wander plus one slow oscillation (~0.5-2 minutes) standing in
    # for respiratory / rest-state variation, a few bpm in amplitude.
    t = np.arange(n) / FS_HZ
    slow_period = rng.uniform(30.0, 120.0)
    hr = (hr_base
          + ar1(n, rho=0.995, sigma=0.25, rng=rng)
          + rng.uniform(1.0, 3.0) * np.sin(2 * np.pi * t / slow_period))

    spo2 = spo2_base + ar1(n, rho=0.99, sigma=0.06, rng=rng)

    # Drip rate: around the doctor's target, with the drip chamber's real
    # mechanical jitter.
    dpm = target_dpm + ar1(n, rho=0.97, sigma=0.35, rng=rng)

    # IV bag weight: falls with the flow rate (1 g ~ 1 ml).
    start_weight = rng.uniform(480.0, 520.0)
    drain_g_per_s = target_flow_ml_h / 3600.0
    weight = start_weight - np.cumsum(np.full(n, drain_g_per_s))
    weight += ar1(n, rho=0.9, sigma=0.4, rng=rng)   # load cell noise

    df = pd.DataFrame({
        "heart_rate": hr,
        "spo2": spo2,
        "drops_per_min": dpm,
        "weight_g": weight,
    })
    df["label_alarm"] = 0
    df["event"] = "normal"
    return df, drain_g_per_s


def inject_transient(df, rng):
    """A 2-6 second TRANSIENT. label_alarm stays 0: the model MUST ignore it.

    The amplitude is deliberately LARGE (past the clinical limits) - this is
    exactly the case the old instantaneous model false-alarmed on, and the real
    test for a windowed model.
    """
    n = len(df)
    dur = rng.integers(2, 7)
    start = rng.integers(60, max(61, n - dur - 60))
    kind = rng.choice(["hr_spike", "hr_drop", "spo2_dip", "drop_burst", "drop_miss"])

    sl = slice(start, start + dur)
    if kind == "hr_spike":
        df.loc[sl, "heart_rate"] += rng.uniform(45, 75)
    elif kind == "hr_drop":
        df.loc[sl, "heart_rate"] -= rng.uniform(30, 45)
    elif kind == "spo2_dip":
        df.loc[sl, "spo2"] -= rng.uniform(8, 14)
    elif kind == "drop_burst":
        df.loc[sl, "drops_per_min"] += rng.uniform(30, 60)
    else:
        df.loc[sl, "drops_per_min"] *= rng.uniform(0.0, 0.2)

    df.loc[sl, "event"] = f"transient_{kind}"
    return df


def inject_sustained(df, rng, drain_g_per_s):
    """A SUSTAINED event >= 45 seconds with a gradual onset -> label_alarm = 1."""
    n = len(df)
    dur = int(rng.integers(45, 150))
    ramp = int(rng.integers(15, 40))          # onset duration
    start = int(rng.integers(120, max(121, n - dur - 60)))
    kind = rng.choice(["occlusion", "free_flow", "desaturation",
                       "tachycardia", "bradycardia"])

    end = min(n, start + dur)
    idx = np.arange(start, end)
    # Coefficient ramps 0->1 then holds: models the gradual onset.
    w = np.clip((idx - start) / max(ramp, 1), 0.0, 1.0)

    if kind == "occlusion":
        # Drips slow then stop; the weight flattens out (no fluid leaving).
        df.loc[start:end - 1, "drops_per_min"] *= (1.0 - 0.95 * w)
        flat = df.loc[start, "weight_g"]
        df.loc[start:end - 1, "weight_g"] = flat + np.cumsum(
            np.full(len(idx), drain_g_per_s)) * (1.0 - w) - 0.0
    elif kind == "free_flow":
        df.loc[start:end - 1, "drops_per_min"] *= (1.0 + 1.6 * w)
        extra = np.cumsum(np.full(len(idx), drain_g_per_s * 2.2)) * w
        df.loc[start:end - 1, "weight_g"] -= extra
    elif kind == "desaturation":
        df.loc[start:end - 1, "spo2"] -= 12.0 * w
        df.loc[start:end - 1, "heart_rate"] += 14.0 * w   # compensatory tachycardia
    elif kind == "tachycardia":
        df.loc[start:end - 1, "heart_rate"] += rng.uniform(45, 70) * w
    else:
        df.loc[start:end - 1, "heart_rate"] -= rng.uniform(22, 32) * w

    df.loc[start:end - 1, "label_alarm"] = 1
    df.loc[start:end - 1, "event"] = kind
    return df


def clamp_physiology(df):
    """Clamp to the physiologically possible range and the sensors' real resolution."""
    df["spo2"] = df["spo2"].clip(60, 100)
    df["heart_rate"] = df["heart_rate"].clip(25, 220)
    df["drops_per_min"] = df["drops_per_min"].clip(0, 240)
    df["weight_g"] = df["weight_g"].clip(0, 2000)
    # The sensors report integers -> round so the dataset matches the chip.
    for c in ["heart_rate", "spo2", "drops_per_min", "weight_g"]:
        df[c] = df[c].round(0)
    return df


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--patients", type=int, default=120)
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--seed", type=int, default=20260730)
    ap.add_argument("--out", default="dataset/iv_timeseries_1hz.csv")
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    frames = []

    for pid in range(args.patients):
        # The doctor's target differs per case, forcing the model to learn from
        # the RATIO rather than from absolute numbers.
        target_dpm = float(rng.choice([15, 18, 20, 25, 30, 40, 50]))
        target_flow = float(rng.choice([60, 80, 100, 120, 150]))

        df, drain = make_patient(rng, args.minutes, target_dpm, target_flow)

        # ~55% of cases get at least one transient (the kind that must be ignored).
        for _ in range(rng.integers(0, 4)):
            df = inject_transient(df, rng)
        # ~45% of cases get one sustained event (the kind that must alarm).
        if rng.random() < 0.45:
            df = inject_sustained(df, rng, drain)

        df = clamp_physiology(df)
        df["patient_id"] = pid
        df["target_dpm"] = target_dpm
        df["target_flow_ml_h"] = target_flow
        df["t"] = np.arange(len(df)) / FS_HZ
        frames.append(df)

    out = pd.concat(frames, ignore_index=True)
    out.to_csv(args.out, index=False)

    n = len(out)
    print(f"Wrote {args.out}")
    print(f"  total samples   : {n} ({n/3600:.1f} hours @1Hz)")
    print(f"  patients        : {args.patients}")
    print(f"  label_alarm = 1 : {out.label_alarm.sum()} ({100*out.label_alarm.mean():.1f}%)")
    tr = out.event.str.startswith("transient").sum()
    print(f"  transient rows  : {tr} ({100*tr/n:.1f}%) - all with label_alarm = 0")
    print("\nEvent type distribution:")
    print(out.event.value_counts().to_string())


if __name__ == "__main__":
    main()
