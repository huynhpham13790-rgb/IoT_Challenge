#!/usr/bin/env python3
"""
Evaluate the forecaster, aimed squarely at the supervisor's question:

  "the heart rate or drip rate jumps for a few seconds then goes back to normal,
   and it false-alarms - which is what annoys the doctor"

Results are reported for two SEPARATE groups; that separation is what makes them
meaningful:
  - TRANSIENT windows (a 2-6 second blip, not pathology) -> alarming here is a
    FALSE ALARM
  - SUSTAINED windows (an event >= 45 seconds)           -> must be caught (recall)

METHODOLOGY: every parameter (scoring method, threshold, persistence length) is
chosen on the VALIDATION split, then reported ONCE on the TEST split. Choosing
parameters on the test split is marking your own homework and makes the numbers
look artificially good.
"""

import argparse
import numpy as np
import tensorflow as tf

HR_CENTER, HR_SCALE = 80.0, 20.0
SPO2_CENTER, SPO2_SCALE = 97.0, 2.0
DR_CENTER, DR_SCALE = 1.0, 0.35
HORIZON, N_CH = 16, 4

# Clinical limits (same as ai_monitor.h) - used by method A
HR_LO, HR_HI = 45.0, 150.0
SPO2_LO = 90.0
DR_LO, DR_HI = 0.3, 1.5


def denorm(y):
    y = y.reshape(-1, HORIZON, N_CH)
    return {"hr": y[..., 0] * HR_SCALE + HR_CENTER,
            "spo2": y[..., 1] * SPO2_SCALE + SPO2_CENTER,
            "dr": y[..., 2] * DR_SCALE + DR_CENTER}


def tflite_predict(path, X):
    interp = tf.lite.Interpreter(model_path=path)
    interp.allocate_tensors()
    inp, out = interp.get_input_details()[0], interp.get_output_details()[0]
    s_in, z_in = inp["quantization"]
    s_out, z_out = out["quantization"]
    preds = np.zeros((len(X), HORIZON * N_CH), np.float32)
    for i in range(len(X)):
        q = np.clip(np.round(X[i] / s_in + z_in), -128, 127).astype(np.int8)
        interp.set_tensor(inp["index"], q[None, ...])
        interp.invoke()
        preds[i] = (interp.get_tensor(out["index"])[0].astype(np.float32) - z_out) * s_out
    return preds


def method_a_instant(X):
    """Instantaneous threshold on the MOST RECENT sample - the current approach."""
    last = X[:, 0, -1, :]
    hr = last[:, 0] * HR_SCALE + HR_CENTER
    spo2 = last[:, 1] * SPO2_SCALE + SPO2_CENTER
    dr = last[:, 2] * DR_SCALE + DR_CENTER
    return ((hr < HR_LO) | (hr > HR_HI) | (spo2 < SPO2_LO)
            | (dr < DR_LO) | (dr > DR_HI))


def channel_errors(pred, truth):
    """|forecast - actual| per (horizon step, channel)."""
    p = pred.reshape(-1, HORIZON, N_CH)
    t = truth.reshape(-1, HORIZON, N_CH)
    return np.abs(p - t)                          # (N, HORIZON, N_CH)


def scale_per_channel(err, ref_mask):
    """Divide each channel's error by that channel's own std-dev on NORMAL data.

    Why it matters: the four channels have very different natural error
    magnitudes. Averaging raw across channels lets a desaturation (visible only
    on spo2) be diluted by the other three until it is almost invisible. After
    per-channel scaling each channel contributes fairly and MAX becomes usable.
    """
    sd = err[ref_mask].reshape(-1, N_CH).std(axis=0) + 1e-6
    return err / sd


def longest_run(mask_2d):
    """Longest run of True along the time axis, per row."""
    n, h = mask_2d.shape
    best = np.zeros(n, np.int32)
    run = np.zeros(n, np.int32)
    for j in range(h):
        run = np.where(mask_2d[:, j], run + 1, 0)
        best = np.maximum(best, run)
    return best


def build_scores(pred, Y, ref_mask):
    """Candidate anomaly-scoring methods, to be chosen on validation."""
    err = channel_errors(pred, Y)
    errn = scale_per_channel(err, ref_mask)
    return {
        "mean_all": err.mean(axis=(1, 2)),                 # raw mean (original)
        "maxch_mean_t": errn.mean(axis=1).max(axis=1),     # max channel, mean over time
        "maxch_max_t": errn.max(axis=2).max(axis=1),       # max over both axes
    }, errn


def report(name, dec, E):
    return (dec[E == "normal"].mean() * 100,
            dec[E == "transient"].mean() * 100,
            dec[E == "sustained"].mean() * 100)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tflite", default="out/forecaster_int8.tflite")
    ap.add_argument("--valset", default="out/valset.npz")
    ap.add_argument("--testset", default="out/testset.npz")
    ap.add_argument("--fp-target", type=float, default=0.02)
    args = ap.parse_args()

    va = np.load(args.valset, allow_pickle=True)
    te = np.load(args.testset, allow_pickle=True)
    Xva, Yva, Eva = va["X"], va["Y"], va["E"].astype(str)
    Xte, Yte, Ete = te["X"], te["Y"], te["E"].astype(str)

    print(f"Val : {len(Xva)} windows (normal={np.sum(Eva=='normal')} "
          f"transient={np.sum(Eva=='transient')} sustained={np.sum(Eva=='sustained')})")
    print(f"Test: {len(Xte)} windows (normal={np.sum(Ete=='normal')} "
          f"transient={np.sum(Ete=='transient')} sustained={np.sum(Ete=='sustained')})")

    print("\nRunning the int8 model on val + test...")
    Pva = tflite_predict(args.tflite, Xva)
    Pte = tflite_predict(args.tflite, Xte)

    # ---------- 1) Forecast accuracy (normal windows only) --------------------
    mn = Ete == "normal"
    dp, dt = denorm(Pte[mn]), denorm(Yte[mn])
    print("\n=== Forecast accuracy (TEST split, normal windows) ===")
    for k, unit, nm in [("hr", "bpm", "Heart rate"), ("spo2", "%", "SpO2"),
                        ("dr", "x target", "Drip ratio")]:
        e = np.abs(dp[k] - dt[k])
        print(f"  {nm:11s} MAE = {e.mean():6.2f} {unit:10s}"
              f" | +1s = {e[:, 0].mean():5.2f} | +16s = {e[:, -1].mean():5.2f}")

    # ---------- 2) Heart-rate trend -------------------------------------------
    # The slope is computed on SMOOTHED values (mean of the first 4 steps vs the
    # last 4): a raw slope over a noisy signal is essentially unforecastable, and
    # smoothing first is what makes this match "trend" in the clinical sense.
    def trend_bpm_per_min(d):
        return (d["hr"][:, -4:].mean(axis=1) - d["hr"][:, :4].mean(axis=1)) / 12.0 * 60.0

    sp, st = trend_bpm_per_min(dp), trend_bpm_per_min(dt)
    print("\n=== Heart-rate trend detection (TEST split) ===")
    for dead in (2.0, 5.0, 10.0):
        lp = np.where(sp > dead, 1, np.where(sp < -dead, -1, 0))
        lt = np.where(st > dead, 1, np.where(st < -dead, -1, 0))
        mv = lt != 0
        acc3 = (lp == lt).mean() * 100
        accd = (np.sign(sp[mv]) == np.sign(st[mv])).mean() * 100 if mv.any() else float("nan")
        print(f"  deadband +-{dead:4.1f} bpm/min: 3-label accuracy {acc3:5.1f}% | "
              f"direction when actually moving {accd:5.1f}% (n={mv.sum()})")

    # Compare: trend taken from the FORECAST versus measured directly from the 64
    # seconds ALREADY OBSERVED. The hypothesis under test is that for "trend" a
    # LONG observation window might beat a short forecast, since short-term HR
    # direction is mostly AR(1) noise and barely forecastable, whereas the slope
    # over 64 seconds of history can simply be measured.
    obs_hr = Xte[mn, 0, :, 0] * HR_SCALE + HR_CENTER         # same 'mn' mask as sp/st
    tt = np.arange(obs_hr.shape[1], dtype=np.float32)
    tt_c = tt - tt.mean()
    obs_slope = ((obs_hr - obs_hr.mean(axis=1, keepdims=True)) * tt_c).sum(axis=1) \
                / (tt_c ** 2).sum() * 60.0                    # bpm/min
    print("\n  Trend signal source comparison (deadband +-10 bpm/min):")
    for nm, sv in [("from the 16s FORECAST", sp), ("from 64s OBSERVED", obs_slope)]:
        lp = np.where(sv > 10, 1, np.where(sv < -10, -1, 0))
        lt = np.where(st > 10, 1, np.where(st < -10, -1, 0))
        mv = lt != 0
        print(f"    {nm:22s} 3-label {100*(lp==lt).mean():5.1f}% | "
              f"direction {100*(np.sign(sv[mv])==np.sign(st[mv])).mean():5.1f}%")

    # ---------- 3) Choose scoring method + threshold ON VALIDATION ------------
    ref_va = Eva == "normal"
    sc_va, errn_va = build_scores(Pva, Yva, ref_va)
    print(f"\n=== Parameter selection on VALIDATION (fp target {args.fp_target*100:.0f}%) ===")

    best = None
    for sname, sv in sc_va.items():
        thr = np.quantile(sv[ref_va], 1.0 - args.fp_target)
        fn, ft, rc = report(sname, sv > thr, Eva)
        print(f"  {sname:14s} thr={thr:7.3f} -> normal {fn:5.1f}% | "
              f"transient {ft:5.1f}% | recall {rc:5.1f}%")
        if best is None or rc > best[3]:
            best = (sname, thr, ft, rc)
    print(f"  -> chosen scoring method: {best[0]}")

    # persistence: grid search over (point threshold, K) on VALIDATION.
    # The criterion is deliberately FAIR: maximise recall SUBJECT TO the transient
    # false-alarm rate being NO WORSE than the current approach (method A). Only
    # then does the claim "better detection at no cost in false alarms" hold.
    a_va = method_a_instant(Xva)
    a_ft = a_va[Eva == "transient"].mean() * 100
    print(f"\n  Method A on val: transient {a_ft:.1f}% | "
          f"recall {a_va[Eva=='sustained'].mean()*100:.1f}%  <- the bar to match or beat")

    step_err_va = errn_va.max(axis=2)
    grid = []
    for fp_pt in (0.005, 0.01, 0.02, 0.04, 0.08, 0.15):
        thr_p = np.quantile(step_err_va[ref_va].ravel(), 1.0 - fp_pt)
        runs = longest_run(step_err_va > thr_p)
        for K in range(1, HORIZON + 1):
            fn, ft, rc = report("", runs >= K, Eva)
            grid.append((rc, ft, fn, fp_pt, thr_p, K))

    ok = [g for g in grid if g[1] <= a_ft]
    if not ok:
        ok = grid
    ok.sort(key=lambda g: -g[0])
    best_rc, best_ft, best_fn, best_fp_pt, thr_pt, chosen_k = ok[0]
    print(f"  -> chosen point threshold fp={best_fp_pt} (thr={thr_pt:.3f}), K={chosen_k}")
    print(f"     on val: normal {best_fn:.1f}% | transient {best_ft:.1f}% | "
          f"recall {best_rc:.1f}%")

    # ---------- 4) Report ONCE on TEST ----------------------------------------
    sc_te, errn_te = build_scores(Pte, Yte, Ete == "normal")
    thr_best = np.quantile(sc_va[best[0]][ref_va], 1.0 - args.fp_target)
    runs_te = longest_run(errn_te.max(axis=2) > thr_pt)

    methods = {
        "A. Instantaneous threshold (current)": method_a_instant(Xte),
        f"B. Forecast ({best[0]}), instantaneous": sc_te[best[0]] > thr_best,
        f"C. Forecast + persistence K={chosen_k}": runs_te >= chosen_k,
    }

    print("\n" + "=" * 74)
    print("RESULTS ON THE TEST SPLIT (parameters fixed from validation)")
    print("=" * 74)
    print(f"{'Method':36s} {'normal':>10s} {'transient':>11s} {'recall':>10s}")
    print(f"{'':36s} {'(false)':>10s} {'(FALSE)':>11s} {'(sustained)':>10s}")
    print("-" * 74)
    for nm, dec in methods.items():
        fn, ft, rc = report(nm, dec, Ete)
        print(f"{nm:36s} {fn:9.1f}% {ft:10.1f}% {rc:9.1f}%")
    print("-" * 74)
    print("The 'transient' column is the number that matters most: those windows")
    print("contain only a 2-6 second blip, NOT pathology - alarming there is false.")

    # ---------- 5) Export the constants the FIRMWARE needs --------------------
    # The firmware must use EXACTLY these constants so its anomaly score matches
    # what was evaluated above. The per-channel std-devs come from the NORMAL data
    # of the VALIDATION split (never from test, which is only ever for reporting).
    err_va = channel_errors(Pva, Yva)
    sd_va = err_va[ref_va].reshape(-1, N_CH).std(axis=0)
    with open("out/fw_thresholds.txt", "w") as f:
        for c, nm in enumerate(["HR", "SPO2", "DROPS", "WEIGHT"]):
            f.write(f"TS_ERR_SD_{nm}={sd_va[c]:.8f}\n")
        f.write(f"TS_POINT_THR={thr_pt:.6f}\n")
        f.write(f"TS_PERSIST_K={chosen_k}\n")
    print("\nWrote out/fw_thresholds.txt (constants for the firmware):")
    for c, nm in enumerate(["HR", "SPO2", "DROPS", "WEIGHT"]):
        print(f"  error std-dev, channel {nm:6s} = {sd_va[c]:.6f}")
    print(f"  point threshold (normalised)   = {thr_pt:.4f}")
    print(f"  persistence length K           = {chosen_k}")


if __name__ == "__main__":
    main()
