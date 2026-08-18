#!/usr/bin/env python3
"""
Generate a HYBRID dataset: HR/SpO2 from REAL ICU PATIENTS (BIDMC), infusion
channels simulated.

Why hybrid:
  - HR and SpO2: BIDMC ships 53 recordings of REAL ICU patients at Beth Israel
    Deaconess, 8 minutes each, sampled at 1 Hz - exactly the time-series format
    the model needs. The project previously used BIDMC only to "calibrate value
    ranges", i.e. it discarded the time axis of a genuinely real dataset - the
    very kind of waste the supervisor criticised. Here the full series is used.
  - Drips / weight: there is NO public labelled dataset for IV infusion, so this
    part must be simulated - but it is attached to REAL vitals series, so the
    overall dynamics still have a real basis.

Abnormal events are INJECTED into the real series, still split into the same two
categories as the synthetic generator:
  - TRANSIENT (2-6 seconds)  -> label_alarm = 0  (must NOT alarm)
  - SUSTAINED (>= 45 seconds) -> label_alarm = 1  (MUST alarm)

Source: BIDMC PPG and Respiration Dataset, PhysioNet
        https://physionet.org/content/bidmc/1.0.0/
        Pimentel et al., IEEE Trans. Biomed. Eng. 64(8):1914-1923, 2016
"""

import argparse
import glob
import os
import numpy as np
import pandas as pd

FS_HZ = 1.0


def load_bidmc(bidmc_dir):
    """Read the *_Numerics.csv files -> list of cleaned 1 Hz HR/SpO2 DataFrames."""
    recs = []
    for path in sorted(glob.glob(os.path.join(bidmc_dir, "*_Numerics.csv"))):
        df = pd.read_csv(path)
        df.columns = [c.strip() for c in df.columns]
        if not {"HR", "SpO2"} <= set(df.columns):
            continue

        hr = pd.to_numeric(df["HR"], errors="coerce")
        spo2 = pd.to_numeric(df["SpO2"], errors="coerce")

        # BIDMC has gaps (NaN) wherever the monitor lost signal. Linearly
        # interpolate the SHORT ones; drop any recording missing too much, since
        # the model must learn continuous dynamics, not patched-up data.
        if hr.isna().mean() > 0.2 or spo2.isna().mean() > 0.2:
            continue
        hr = hr.interpolate(limit=5).ffill().bfill()
        spo2 = spo2.interpolate(limit=5).ffill().bfill()
        if hr.isna().any() or spo2.isna().any():
            continue

        # Remove any remaining physiologically impossible values.
        if not (hr.between(25, 220).all() and spo2.between(60, 100).all()):
            hr = hr.clip(25, 220)
            spo2 = spo2.clip(60, 100)

        recs.append(pd.DataFrame({
            "heart_rate": hr.to_numpy(np.float64),
            "spo2": spo2.to_numpy(np.float64),
            "src": os.path.basename(path),
        }))
    return recs


def ar1(n, rho, sigma, rng):
    x = np.zeros(n, dtype=np.float64)
    for t in range(1, n):
        x[t] = rho * x[t - 1] + rng.normal(0.0, sigma)
    return x


def add_iv_channels(df, rng, target_dpm, target_flow_ml_h):
    """Attach the two simulated infusion channels to the real vitals series."""
    n = len(df)
    df = df.copy()
    df["drops_per_min"] = target_dpm + ar1(n, rho=0.97, sigma=0.35, rng=rng)

    start_w = rng.uniform(480.0, 520.0)
    drain = target_flow_ml_h / 3600.0          # g/second, assuming 1 g ~ 1 ml
    df["weight_g"] = start_w - np.cumsum(np.full(n, drain)) \
                     + ar1(n, rho=0.9, sigma=0.4, rng=rng)
    df["label_alarm"] = 0
    df["event"] = "normal"
    return df, drain


def inject_transient(df, rng):
    """A 2-6 second transient on the REAL series. label_alarm stays 0."""
    n = len(df)
    dur = int(rng.integers(2, 7))
    lo, hi = 60, max(61, n - dur - 60)
    if hi <= lo:
        return df
    start = int(rng.integers(lo, hi))
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


def inject_sustained(df, rng, drain):
    """A sustained event >= 45 seconds with a gradual onset -> label_alarm = 1."""
    n = len(df)
    dur = int(rng.integers(45, 150))
    ramp = int(rng.integers(15, 40))
    lo, hi = 120, max(121, n - dur - 60)
    if hi <= lo:
        return df
    start = int(rng.integers(lo, hi))
    end = min(n, start + dur)
    idx = np.arange(start, end)
    w = np.clip((idx - start) / max(ramp, 1), 0.0, 1.0)
    kind = rng.choice(["occlusion", "free_flow", "desaturation",
                       "tachycardia", "bradycardia"])

    if kind == "occlusion":
        df.loc[start:end - 1, "drops_per_min"] *= (1.0 - 0.95 * w)
        flat = df.loc[start, "weight_g"]
        df.loc[start:end - 1, "weight_g"] = \
            flat + np.cumsum(np.full(len(idx), drain)) * (1.0 - w)
    elif kind == "free_flow":
        df.loc[start:end - 1, "drops_per_min"] *= (1.0 + 1.6 * w)
        df.loc[start:end - 1, "weight_g"] -= \
            np.cumsum(np.full(len(idx), drain * 2.2)) * w
    elif kind == "desaturation":
        df.loc[start:end - 1, "spo2"] -= 12.0 * w
        df.loc[start:end - 1, "heart_rate"] += 14.0 * w
    elif kind == "tachycardia":
        df.loc[start:end - 1, "heart_rate"] += rng.uniform(45, 70) * w
    else:
        df.loc[start:end - 1, "heart_rate"] -= rng.uniform(22, 32) * w

    df.loc[start:end - 1, "label_alarm"] = 1
    df.loc[start:end - 1, "event"] = kind
    return df


def clamp(df):
    df["spo2"] = df["spo2"].clip(60, 100)
    df["heart_rate"] = df["heart_rate"].clip(25, 220)
    df["drops_per_min"] = df["drops_per_min"].clip(0, 240)
    df["weight_g"] = df["weight_g"].clip(0, 2000)
    for c in ["heart_rate", "spo2", "drops_per_min", "weight_g"]:
        df[c] = df[c].round(0)
    return df


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bidmc-dir", default="dataset/bidmc")
    ap.add_argument("--out", default="dataset/iv_hybrid_1hz.csv")
    ap.add_argument("--repeats", type=int, default=3,
                    help="how many times to reuse each BIDMC recording with different events")
    ap.add_argument("--seed", type=int, default=20260730)
    args = ap.parse_args()

    recs = load_bidmc(args.bidmc_dir)
    if not recs:
        raise SystemExit(f"No usable BIDMC recording found in {args.bidmc_dir}")
    print(f"Loaded {len(recs)} usable BIDMC recordings "
          f"(mean {np.mean([len(r) for r in recs]):.0f} samples each)")

    rng = np.random.default_rng(args.seed)
    frames = []
    pid = 0

    for rep in range(args.repeats):
        for rec in recs:
            target_dpm = float(rng.choice([15, 18, 20, 25, 30, 40, 50]))
            target_flow = float(rng.choice([60, 80, 100, 120, 150]))

            df, drain = add_iv_channels(rec.drop(columns=["src"]), rng,
                                        target_dpm, target_flow)
            for _ in range(int(rng.integers(0, 4))):
                df = inject_transient(df, rng)
            if rng.random() < 0.45:
                df = inject_sustained(df, rng, drain)

            df = clamp(df)
            # EACH reuse counts as a separate "case" for splitting purposes, but
            # every case derived from the SAME BIDMC recording keeps its source
            # id so the split can be made per REAL PERSON - otherwise the same
            # patient would leak across train and test.
            df["patient_id"] = pid
            df["bidmc_src"] = rec["src"].iloc[0]
            df["target_dpm"] = target_dpm
            df["target_flow_ml_h"] = target_flow
            df["t"] = np.arange(len(df)) / FS_HZ
            frames.append(df)
            pid += 1

    out = pd.concat(frames, ignore_index=True)
    out.to_csv(args.out, index=False)

    n = len(out)
    print(f"\nWrote {args.out}")
    print(f"  total samples   : {n} ({n/3600:.1f} hours @1Hz)")
    print(f"  cases           : {pid} ({len(recs)} real BIDMC patients x {args.repeats})")
    print(f"  label_alarm = 1 : {out.label_alarm.sum()} ({100*out.label_alarm.mean():.1f}%)")
    tr = out.event.str.startswith("transient").sum()
    print(f"  transient rows  : {tr} ({100*tr/n:.1f}%) - all with label_alarm = 0")
    print(f"\n  real HR  : {out.heart_rate.min():.0f}-{out.heart_rate.max():.0f} bpm "
          f"(median {out.heart_rate.median():.0f})")
    print(f"  real SpO2: {out.spo2.min():.0f}-{out.spo2.max():.0f} % "
          f"(median {out.spo2.median():.0f})")
    print("\nEvent type distribution:")
    print(out.event.value_counts().to_string())


if __name__ == "__main__":
    main()
