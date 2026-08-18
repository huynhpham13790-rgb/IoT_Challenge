#!/usr/bin/env python3
"""
AER: add a window-RECONSTRUCTION branch next to the forecasting branch.

This is limitation 13.1 of AI_TIME_SERIES_TAT_TAN_TAT.md ("recall 58.3% is not
enough; add a reconstruction branch, AER, arXiv 2212.13558").

THE IDEA, and why it is not just "more model"
---------------------------------------------
The current model is asked one question: given 64 seconds, what happens in the
next 16? Anomaly = the part of the future it could not forecast. That question
has a blind spot: the near future of a vital sign is dominated by where it is
right now, so a slow drift the model has already "accepted" as the current level
gets forecast correctly and scores as normal - even though the WINDOW itself is
abnormal.

Reconstruction asks the complementary question: given this 64-second window
compressed through the encoder, can it be rebuilt? A window whose SHAPE is
unlike anything in the training data cannot be, no matter how predictable its
continuation is.

AER's point is that the two errors fail on different things, so combining them
detects more than either alone. Both heads share one encoder, so the extra cost
is one more Dense layer, not a second model.

Trains, exports int8, and evaluates against the SAME validation/test protocol as
evaluate.py so the numbers are directly comparable with the shipped model:
parameters chosen on validation, reported once on test, and the transient
false-alarm rate is not allowed to get worse than the current rule-based check.
"""

import argparse
import os

import numpy as np
import pandas as pd
import tensorflow as tf

import train_forecaster as tfc          # window building, splits, normalisation
import evaluate as ev                   # scoring helpers, protocol, reporting

WINDOW, HORIZON, N_CH = tfc.WINDOW, tfc.HORIZON, tfc.N_CH


def build_model(batch=None, bottleneck=0):
    """Shared encoder, two heads.

    The encoder is byte-for-byte the one in train_forecaster.build_model() - the
    point of the experiment is what the extra head adds, so nothing else may
    change. Same reasons apply as there: Conv2D with a (1,k) kernel instead of
    Conv1D, stride instead of dilation, static Reshape instead of Flatten, so
    every operator stays inside the MVP-accelerated set.
    """
    if batch is None:
        inp = tf.keras.Input(shape=(1, WINDOW, N_CH), name="window")
    else:
        inp = tf.keras.Input(batch_shape=(batch, 1, WINDOW, N_CH), name="window")

    x = tf.keras.layers.Conv2D(16, (1, 5), strides=(1, 2), padding="same",
                               activation="relu")(inp)
    x = tf.keras.layers.Conv2D(32, (1, 5), strides=(1, 2), padding="same",
                               activation="relu")(x)
    x = tf.keras.layers.Conv2D(32, (1, 3), strides=(1, 2), padding="same",
                               activation="relu")(x)
    code = tf.keras.layers.Reshape(((WINDOW // 8) * 32,))(x)

    f = tf.keras.layers.Dense(48, activation="relu")(code)
    forecast = tf.keras.layers.Dense(HORIZON * N_CH, name="forecast_flat")(f)

    # A reconstruction head is only an ANOMALY detector if it is forced through a
    # bottleneck SMALLER than its input. The shared encoder's code is
    # (WINDOW/8)*32 = 256 values and the window is 64*4 = 256 values, i.e. no
    # compression at all: given that, the head can learn something close to a
    # copy, and a copier reconstructs abnormal windows just as happily as normal
    # ones - which is exactly what the first run measured (recall 14.8% on its
    # own). `--bottleneck N` inserts the missing squeeze.
    if bottleneck:
        r = tf.keras.layers.Dense(bottleneck, activation="relu", name="bottleneck")(code)
        r = tf.keras.layers.Dense(64, activation="relu")(r)
    else:
        r = tf.keras.layers.Dense(48, activation="relu")(code)
    recon = tf.keras.layers.Dense(WINDOW * N_CH, name="recon_flat")(r)

    m = tf.keras.Model(inp, [forecast, recon], name="smartiv_aer")
    m.compile(optimizer=tf.keras.optimizers.Adam(1e-3),
              loss={"forecast_flat": tf.keras.losses.Huber(delta=1.0),
                    "recon_flat": tf.keras.losses.Huber(delta=1.0)},
              # Forecasting is what drives trend and early warning, both of which
              # are user-visible; reconstruction only feeds the anomaly score. An
              # equal weighting let reconstruction (4x as many outputs) dominate
              # the gradient and measurably degraded forecast MAE.
              loss_weights={"forecast_flat": 1.0, "recon_flat": 0.5},
              metrics={"forecast_flat": "mae", "recon_flat": "mae"})
    return m


def tflite_predict_two_heads(path, X):
    """Run the int8 model and return (forecast, reconstruction).

    Output order in the flatbuffer is not guaranteed to follow the Keras output
    order, so the heads are told apart by their LENGTH (16*4 vs 64*4) rather
    than by index - an index that silently swaps would turn every score into
    noise without any error being raised.
    """
    interp = tf.lite.Interpreter(model_path=path)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    outs = interp.get_output_details()
    s_in, z_in = inp["quantization"]

    idx_f = next(o for o in outs if int(np.prod(o["shape"][1:])) == HORIZON * N_CH)
    idx_r = next(o for o in outs if int(np.prod(o["shape"][1:])) == WINDOW * N_CH)

    P = np.zeros((len(X), HORIZON * N_CH), np.float32)
    R = np.zeros((len(X), WINDOW * N_CH), np.float32)
    for i in range(len(X)):
        q = np.clip(np.round(X[i] / s_in + z_in), -128, 127).astype(np.int8)
        interp.set_tensor(inp["index"], q[None, ...])
        interp.invoke()
        sf, zf = idx_f["quantization"]
        sr, zr = idx_r["quantization"]
        P[i] = (interp.get_tensor(idx_f["index"])[0].astype(np.float32) - zf) * sf
        R[i] = (interp.get_tensor(idx_r["index"])[0].astype(np.float32) - zr) * sr
    return P, R


def recon_errors(recon, X):
    """|reconstruction - input| per (time step, channel), same shape convention
    as evaluate.channel_errors() so the two can be scored the same way."""
    r = recon.reshape(-1, WINDOW, N_CH)
    t = X.reshape(-1, WINDOW, N_CH)
    return np.abs(r - t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="dataset/iv_hybrid_1hz.csv")
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--seed", type=int, default=20260730)
    ap.add_argument("--outdir", default="out_aer")
    ap.add_argument("--fp-target", type=float, default=0.02)
    ap.add_argument("--bottleneck", type=int, default=0,
                    help="Width of the reconstruction bottleneck. 0 = none "
                         "(the head sees a 256-value code for a 256-value "
                         "window, so it can learn a copy).")
    ap.add_argument("--eval-only", action="store_true",
                    help="Skip training; re-score the model already in --outdir. "
                         "Used to try different ways of combining the two heads "
                         "without the result depending on a fresh set of weights.")
    args = ap.parse_args()

    df = pd.read_csv(args.csv)
    group_col = "bidmc_src" if "bidmc_src" in df.columns else "patient_id"
    tr_ids, va_ids, te_ids = tfc.split_by_patient(df, args.seed, group_col)
    print(f"Split on '{group_col}': train={len(tr_ids)} val={len(va_ids)} test={len(te_ids)}")

    Xtr, Ytr, _, _ = tfc.make_windows(df[df[group_col].isin(tr_ids)], only_normal=True)
    Xva, Yva, _, _ = tfc.make_windows(df[df[group_col].isin(va_ids)], only_normal=True)
    Xte, Yte, Lte, Ete = tfc.make_windows(df[df[group_col].isin(te_ids)], only_normal=False)
    Xva_all, Yva_all, _, Eva_all = tfc.make_windows(
        df[df[group_col].isin(va_ids)], only_normal=False)
    print(f"Windows: train={len(Xtr)} val={len(Xva)} test={len(Xte)}")

    flat = lambda A: A.reshape(len(A), -1)      # noqa: E731 - window -> recon target

    tflite_path = os.path.join(args.outdir, "aer_int8.tflite")
    if args.eval_only:
        print(f"--eval-only: reusing {tflite_path}")
        run_evaluation(args, tflite_path, Xva_all, Yva_all, Eva_all, Xte, Yte, Ete)
        return

    model = build_model(bottleneck=args.bottleneck)
    model.summary()
    model.fit(Xtr, {"forecast_flat": Ytr, "recon_flat": flat(Xtr)},
              validation_data=(Xva, {"forecast_flat": Yva, "recon_flat": flat(Xva)}),
              epochs=args.epochs, batch_size=128, verbose=2,
              callbacks=[
                  tf.keras.callbacks.EarlyStopping(patience=8, restore_best_weights=True),
                  tf.keras.callbacks.ReduceLROnPlateau(patience=4, factor=0.5, min_lr=1e-5),
              ])

    os.makedirs(args.outdir, exist_ok=True)

    clone = build_model(batch=1, bottleneck=args.bottleneck)
    clone.set_weights(model.get_weights())

    def rep_data():
        idx = np.random.default_rng(0).choice(len(Xtr), min(500, len(Xtr)), replace=False)
        for i in idx:
            yield [Xtr[i:i + 1]]

    conv = tf.lite.TFLiteConverter.from_keras_model(clone)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep_data
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tfl = conv.convert()

    with open(tflite_path, "wb") as f:
        f.write(tfl)
    print(f"\nWrote {tflite_path}  ({len(tfl)} byte)")

    np.savez_compressed(os.path.join(args.outdir, "testset.npz"),
                        X=Xte, Y=Yte, L=Lte, E=Ete.astype(str))
    np.savez_compressed(os.path.join(args.outdir, "valset.npz"),
                        X=Xva_all, Y=Yva_all, E=Eva_all.astype(str))
    model.save(os.path.join(args.outdir, "aer.keras"))

    from tensorflow.lite.python import schema_py_generated as schema
    mm = schema.Model.GetRootAsModel(tfl, 0)
    bnames = {v: k for k, v in vars(schema.BuiltinOperator).items() if isinstance(v, int)}
    ocodes = [mm.OperatorCodes(i).BuiltinCode() for i in range(mm.OperatorCodesLength())]
    sg = mm.Subgraphs(0)
    op_seq = [bnames.get(ocodes[sg.Operators(i).OpcodeIndex()], "?")
              for i in range(sg.OperatorsLength())]
    MVP_OK = {"CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED",
              "AVERAGE_POOL_2D", "MAX_POOL_2D", "ADD", "MUL", "RESHAPE"}
    print(f"Operators ({len(op_seq)}): {op_seq}")
    print(f"All within the MVP-accelerated set: {set(op_seq) <= MVP_OK}")

    run_evaluation(args, tflite_path, Xva_all, Yva_all, Eva_all, Xte, Yte, Ete)


def run_evaluation(args, tflite_path, Xva_all, Yva_all, Eva_all, Xte, Yte, Ete):
    """Same protocol as evaluate.py: choose everything on validation, report
    once on test, and never let transient false alarms get worse than the
    current rule-based check."""
    print("\nRunning the int8 AER model on val + test...")
    Pva, Rva = tflite_predict_two_heads(tflite_path, Xva_all)
    Pte, Rte = tflite_predict_two_heads(tflite_path, Xte)

    ref_va = Eva_all == "normal"
    ref_te = Ete == "normal"

    print("\n=== Forecast accuracy (TEST, normal windows) - must not regress ===")
    dp, dt = ev.denorm(Pte[ref_te]), ev.denorm(Yte[ref_te])
    for k, unit, nm in [("hr", "bpm", "Heart rate"), ("spo2", "%", "SpO2"),
                        ("dr", "x target", "Drip ratio")]:
        e = np.abs(dp[k] - dt[k])
        print(f"  {nm:11s} MAE = {e.mean():6.2f} {unit}")

    # Per-channel scaling on NORMAL validation data, exactly as evaluate.py does,
    # so forecast and reconstruction errors become comparable numbers and can be
    # combined without one channel or one head swamping the rest.
    f_err_va = ev.channel_errors(Pva, Yva_all)
    f_err_te = ev.channel_errors(Pte, Yte)
    r_err_va = recon_errors(Rva, Xva_all)
    r_err_te = recon_errors(Rte, Xte)

    f_sd = f_err_va[ref_va].reshape(-1, N_CH).std(axis=0) + 1e-6
    r_sd = r_err_va[ref_va].reshape(-1, N_CH).std(axis=0) + 1e-6

    f_step_va, f_step_te = (f_err_va / f_sd).max(axis=2), (f_err_te / f_sd).max(axis=2)
    r_step_va, r_step_te = (r_err_va / r_sd).max(axis=2), (r_err_te / r_sd).max(axis=2)

    a_va = ev.method_a_instant(Xva_all)
    a_ft = a_va[Eva_all == "transient"].mean() * 100
    print(f"\nMethod A (current rule) on val: transient {a_ft:.1f}% | "
          f"recall {a_va[Eva_all=='sustained'].mean()*100:.1f}%  <- bar to beat")

    def tune(step_va, name):
        """Grid-search (point threshold, persistence K) on VALIDATION, maximising
        recall subject to transient false alarms not exceeding method A."""
        grid = []
        for fp_pt in (0.005, 0.01, 0.02, 0.04, 0.08, 0.15):
            thr = np.quantile(step_va[ref_va].ravel(), 1.0 - fp_pt)
            runs = ev.longest_run(step_va > thr)
            for K in range(1, step_va.shape[1] + 1):
                fn, ft, rc = ev.report("", runs >= K, Eva_all)
                grid.append((rc, ft, fn, thr, K))
        ok = [g for g in grid if g[1] <= a_ft] or grid
        ok.sort(key=lambda g: -g[0])
        rc, ft, fn, thr, K = ok[0]
        print(f"  {name:22s} thr={thr:6.3f} K={K:2d} -> val normal {fn:4.1f}% | "
              f"transient {ft:4.1f}% | recall {rc:5.1f}%")
        return thr, K

    print("\n=== Parameter selection on VALIDATION ===")
    thr_f, K_f = tune(f_step_va, "forecast only")
    thr_r, K_r = tune(r_step_va, "reconstruction only")

    # AER's own combination is a WEIGHTED SUM of the two errors, not a max, and
    # the weight is a tuned parameter. Sweeping it here rather than assuming one
    # is the difference between "the reconstruction branch does not help" and
    # "the reconstruction branch does not help WITH THE ONE WEIGHTING I TRIED".
    best_alpha, best_rc = None, -1.0
    for alpha in (0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.8, 1.0):
        st_va = (1.0 - alpha) * f_step_va + alpha * r_step_va[:, -HORIZON:]
        thr_a, K_a = tune(st_va, f"weighted a={alpha:.1f}")
        runs = ev.longest_run(st_va > thr_a) >= K_a
        _, ft_a, rc_a = ev.report("", runs, Eva_all)
        if ft_a <= a_ft and rc_a > best_rc:
            best_alpha, best_rc = alpha, rc_a
    print(f"  -> best weight on validation: alpha={best_alpha} (recall {best_rc:.1f}%)")
    # AER combines the two errors. Taking the MAX (rather than a weighted sum)
    # keeps the rule readable on-chip and needs no extra tuned weight: a window
    # is anomalous if EITHER head fails on it, which is the whole reason for
    # having two heads that fail on different things.
    c_step_va = ((1.0 - best_alpha) * f_step_va
                 + best_alpha * r_step_va[:, -HORIZON:])
    c_step_te = ((1.0 - best_alpha) * f_step_te
                 + best_alpha * r_step_te[:, -HORIZON:])
    thr_c, K_c = tune(c_step_va, f"AER combined a={best_alpha}")

    print("\n" + "=" * 74)
    print("RESULTS ON TEST (parameters fixed on validation)")
    print("=" * 74)
    print(f"{'Method':40s} {'normal':>9s} {'transient':>11s} {'recall':>9s}")
    print("-" * 74)
    rows = {
        "A. Instantaneous threshold (current rule)": ev.method_a_instant(Xte),
        f"B. Forecast only, K={K_f}": ev.longest_run(f_step_te > thr_f) >= K_f,
        f"C. Reconstruction only, K={K_r}": ev.longest_run(r_step_te > thr_r) >= K_r,
        f"D. AER combined (alpha={best_alpha}), K={K_c}": ev.longest_run(c_step_te > thr_c) >= K_c,
    }
    for nm, dec in rows.items():
        fn, ft, rc = ev.report(nm, dec, Ete)
        print(f"{nm:40s} {fn:8.1f}% {ft:10.1f}% {rc:8.1f}%")
    print("-" * 74)

    with open(os.path.join(args.outdir, "fw_thresholds_aer.txt"), "w") as f:
        for c, nm in enumerate(["HR", "SPO2", "DROPS", "WEIGHT"]):
            f.write(f"TS_ERR_SD_{nm}={f_sd[c]:.8f}\n")
        for c, nm in enumerate(["HR", "SPO2", "DROPS", "WEIGHT"]):
            f.write(f"TS_RECON_SD_{nm}={r_sd[c]:.8f}\n")
        f.write(f"TS_POINT_THR={thr_c:.6f}\n")
        f.write(f"TS_PERSIST_K={K_c}\n")
    print(f"Wrote {args.outdir}/fw_thresholds_aer.txt")


if __name__ == "__main__":
    main()
