#!/usr/bin/env python3
"""
Train the time-series FORECASTER for Smart IV and export it as int8 for EFR32xG26.

The idea (following the supervisor's suggestion: "forecast ahead, is the heart
rate trending up or down"):

    input  : past 64 seconds x 4 channels
    output : forecast of the next 16 seconds x 4 channels

A single model yields all three of the following, none of which a threshold can
produce:
    1. TREND        - slope of the forecast -> "HR is climbing/falling X bpm/min"
    2. EARLY WARNING - the forecast crosses a clinical limit BEFORE reality does,
                       giving tens of seconds of notice
    3. ANOMALY      - forecast error (the model only ever learned NORMAL
                       dynamics, so whatever it cannot forecast is abnormal)

The architecture is chosen against the xG26 hardware (verified in SDK aiml 2.2.2):
    - The MVP accelerator covers CONV_2D / DEPTHWISE_CONV_2D / FULLY_CONNECTED /
      pooling / add / mul. TFLite lowers Keras Conv1D to CONV_2D, so it is
      accelerated.
    - LSTM/GRU exist in TFLM but are NOT MVP-accelerated -> not used.
    - The MVP does not support dilation -> use stride, never dilated convolution.
    - Even channel counts (16/32) improve the odds of acceleration.

Normalisation uses FIXED constants (no scaler.json) so the firmware can reproduce
it with a couple of arithmetic operations and no extra config file to load.
"""

import argparse
import numpy as np
import pandas as pd
import tensorflow as tf

WINDOW = 64          # past 64 seconds (even, divisible by 2 repeatedly -> good for striding)
HORIZON = 16         # forecast the next 16 seconds
N_CH = 4             # HR, SpO2, drops_ratio, relative weight (even -> MVP friendly)

# ---- Normalisation: fixed constants, reproducible in firmware with one
# subtraction and one division ------------------------------------------------
HR_CENTER, HR_SCALE = 80.0, 20.0
SPO2_CENTER, SPO2_SCALE = 97.0, 2.0
DR_CENTER, DR_SCALE = 1.0, 0.35        # drops_ratio sits around 1.0
WREL_SCALE = 5.0                        # grams, relative to the window start


def build_channels(df):
    """The 4 input channels, normalised.

    The weight channel uses a value RELATIVE to the window start rather than the
    absolute reading: every IV bag starts around 500 g and drains, so the
    absolute number only tells you how long the infusion has been running - the
    REAL information is in the RATE OF DECREASE. Using a relative value makes the
    model invariant to the starting mass.
    """
    hr = (df["heart_rate"].to_numpy(np.float32) - HR_CENTER) / HR_SCALE
    spo2 = (df["spo2"].to_numpy(np.float32) - SPO2_CENTER) / SPO2_SCALE
    dr = (df["drops_per_min"].to_numpy(np.float32) / df["target_dpm"].to_numpy(np.float32)
          - DR_CENTER) / DR_SCALE
    w = df["weight_g"].to_numpy(np.float32)
    return hr, spo2, dr, w


def make_windows(df, only_normal):
    """Cut sliding windows. Each window is labelled by its FUTURE part (the horizon)."""
    X, Y, lbl, ev = [], [], [], []

    for _, g in df.groupby("patient_id", sort=False):
        g = g.reset_index(drop=True)
        hr, spo2, dr, w = build_channels(g)
        alarm = g["label_alarm"].to_numpy(np.int32)
        event = g["event"].to_numpy(object)
        n = len(g)

        for s in range(0, n - WINDOW - HORIZON):
            e = s + WINDOW
            f = e + HORIZON

            # Weight: converted to a value relative to the first sample.
            w0 = w[s]
            wrel_in = (w[s:e] - w0) / WREL_SCALE
            wrel_out = (w[e:f] - w0) / WREL_SCALE

            xin = np.stack([hr[s:e], spo2[s:e], dr[s:e], wrel_in], axis=-1)[None, ...]
            yout = np.stack([hr[e:f], spo2[e:f], dr[e:f], wrel_out], axis=-1)

            # Window label = does a sustained event occur in the future part.
            fut_alarm = int(alarm[e:f].max())
            # Record which event types appear across window + horizon, so
            # transient and sustained can be scored separately later.
            evs = set(event[s:f])
            has_transient = any(str(x).startswith("transient") for x in evs)

            if only_normal:
                # Train only on COMPLETELY normal dynamics: no sustained event
                # and no transient either. Letting transients in would teach the
                # model that a 60 bpm jump is "normal" and destroy its ability to
                # detect anything - unsupervised learning needs clean data.
                if fut_alarm or has_transient or alarm[s:e].max():
                    continue

            X.append(xin)
            Y.append(yout.reshape(-1))   # flat, matching Dense(HORIZON*N_CH)
            lbl.append(fut_alarm)
            ev.append("transient" if has_transient and not fut_alarm
                      else ("sustained" if fut_alarm else "normal"))

    if not X:
        return (np.zeros((0, 1, WINDOW, N_CH), np.float32),
                np.zeros((0, HORIZON * N_CH), np.float32),
                np.zeros((0,), np.int32), np.array([], dtype=object))

    return (np.asarray(X, np.float32), np.asarray(Y, np.float32),
            np.asarray(lbl, np.int32), np.asarray(ev, dtype=object))


def build_model():
    """1D CNN implemented as Conv2D with height=1, using stride instead of dilation.

    IMPORTANT - why NOT Keras Conv1D: TFLite lowers every Conv1D layer into three
    operators, EXPAND_DIMS -> CONV_2D -> RESHAPE. With 3 conv layers the model
    balloons to 15 operators of which only 5 are MVP-accelerated; the rest is
    pure overhead. Using Conv2D with a (1,k) kernel directly yields exactly 6:
        CONV_2D x3, RESHAPE x1, FULLY_CONNECTED x2
    all of them in the MVP-accelerated set (RESHAPE in TFLM is only a change of
    memory view and costs no arithmetic).

    The input is shaped (height=1, width=WINDOW, channels=N_CH) - an image one
    pixel tall.
    """
    inp = tf.keras.Input(shape=(1, WINDOW, N_CH), name="window")

    x = tf.keras.layers.Conv2D(16, (1, 5), strides=(1, 2), padding="same",
                               activation="relu")(inp)
    x = tf.keras.layers.Conv2D(32, (1, 5), strides=(1, 2), padding="same",
                               activation="relu")(x)
    x = tf.keras.layers.Conv2D(32, (1, 3), strides=(1, 2), padding="same",
                               activation="relu")(x)
    # Reshape with a STATIC size (not Flatten): Flatten with a dynamic batch
    # emits SHAPE/STRIDED_SLICE/PACK in TFLite - more unaccelerated operators.
    x = tf.keras.layers.Reshape(((WINDOW // 8) * 32,))(x)
    x = tf.keras.layers.Dense(48, activation="relu")(x)
    out = tf.keras.layers.Dense(HORIZON * N_CH, name="forecast_flat")(x)

    m = tf.keras.Model(inp, out, name="smartiv_forecaster")
    m.compile(optimizer=tf.keras.optimizers.Adam(1e-3),
              loss=tf.keras.losses.Huber(delta=1.0),   # more outlier-robust than MSE
              metrics=["mae"])
    return m


def build_fixed_batch_clone(trained):
    """An identical architecture with a FIXED batch of 1, used ONLY for conversion.

    Why it is needed: TFLite can only fold away the dynamic-shape operators when
    every shape is static, but training with a fixed batch of 1 is extremely
    slow. So: train with a dynamic batch, then copy the weights into a batch-1
    clone for conversion. (Going through `from_concrete_functions` instead fails
    with a READ_VARIABLE error because the Keras variables are not initialised in
    that graph.)
    """
    inp = tf.keras.Input(batch_shape=(1, 1, WINDOW, N_CH), name="window")
    x = tf.keras.layers.Conv2D(16, (1, 5), strides=(1, 2), padding="same",
                               activation="relu")(inp)
    x = tf.keras.layers.Conv2D(32, (1, 5), strides=(1, 2), padding="same",
                               activation="relu")(x)
    x = tf.keras.layers.Conv2D(32, (1, 3), strides=(1, 2), padding="same",
                               activation="relu")(x)
    x = tf.keras.layers.Reshape(((WINDOW // 8) * 32,))(x)
    x = tf.keras.layers.Dense(48, activation="relu")(x)
    out = tf.keras.layers.Dense(HORIZON * N_CH, name="forecast_flat")(x)
    clone = tf.keras.Model(inp, out)
    clone.set_weights(trained.get_weights())
    return clone


def split_by_patient(df, seed, group_col="patient_id"):
    """Split by PATIENT, never by row - otherwise data leaks between splits.

    For the hybrid dataset (BIDMC) group_col must be 'bidmc_src', NOT
    'patient_id': each BIDMC recording is reused several times with different
    injected events, so splitting on 'patient_id' would put the SAME real patient
    in both train and test - the model would already have seen that person's
    HR/SpO2 dynamics and the evaluation would look artificially good.
    """
    pids = np.array(sorted(df[group_col].unique()))
    rng = np.random.default_rng(seed)
    rng.shuffle(pids)
    n = len(pids)
    n_tr, n_va = int(0.6 * n), int(0.2 * n)
    return (set(pids[:n_tr].tolist()),
            set(pids[n_tr:n_tr + n_va].tolist()),
            set(pids[n_tr + n_va:].tolist()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="dataset/iv_timeseries_1hz.csv")
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--seed", type=int, default=20260730)
    ap.add_argument("--outdir", default="out")
    args = ap.parse_args()

    df = pd.read_csv(args.csv)
    # The hybrid dataset carries a 'bidmc_src' column -> split by REAL patient.
    group_col = "bidmc_src" if "bidmc_src" in df.columns else "patient_id"
    tr_ids, va_ids, te_ids = split_by_patient(df, args.seed, group_col)
    print(f"Split on '{group_col}': train={len(tr_ids)} val={len(va_ids)} "
          f"test={len(te_ids)}")

    # Train/val: normal dynamics ONLY (unsupervised learning).
    Xtr, Ytr, _, _ = make_windows(df[df[group_col].isin(tr_ids)], only_normal=True)
    Xva, Yva, _, _ = make_windows(df[df[group_col].isin(va_ids)], only_normal=True)
    # Test: keep everything, so transient and sustained can be scored separately.
    Xte, Yte, Lte, Ete = make_windows(df[df[group_col].isin(te_ids)], only_normal=False)
    # A FULL validation split (abnormalities included): used to CHOOSE the
    # scoring method and thresholds. Never choose those on the test split - that
    # would be marking your own homework.
    Xva_all, Yva_all, _, Eva_all = make_windows(
        df[df[group_col].isin(va_ids)], only_normal=False)

    print(f"Windows: train={len(Xtr)} val={len(Xva)} test={len(Xte)}")
    print(f"  test: normal={(Ete=='normal').sum()} "
          f"transient={(Ete=='transient').sum()} sustained={(Ete=='sustained').sum()}")

    model = build_model()
    model.summary()

    model.fit(Xtr, Ytr, validation_data=(Xva, Yva),
              epochs=args.epochs, batch_size=128, verbose=2,
              callbacks=[
                  tf.keras.callbacks.EarlyStopping(patience=8, restore_best_weights=True),
                  tf.keras.callbacks.ReduceLROnPlateau(patience=4, factor=0.5, min_lr=1e-5),
              ])

    # ---- Xuat int8 ------------------------------------------------------------
    def rep_data():
        idx = np.random.default_rng(0).choice(len(Xtr), min(500, len(Xtr)), replace=False)
        for i in idx:
            yield [Xtr[i:i + 1]]

    conv = tf.lite.TFLiteConverter.from_keras_model(build_fixed_batch_clone(model))
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep_data
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tfl = conv.convert()

    import os
    os.makedirs(args.outdir, exist_ok=True)
    tflite_path = os.path.join(args.outdir, "forecaster_int8.tflite")
    with open(tflite_path, "wb") as f:
        f.write(tfl)
    print(f"\nWrote {tflite_path}  ({len(tfl)} byte)")

    # Save the test data and the float model so the evaluation script can reuse them.
    np.savez_compressed(os.path.join(args.outdir, "testset.npz"),
                        X=Xte, Y=Yte, L=Lte, E=Ete.astype(str))
    np.savez_compressed(os.path.join(args.outdir, "valset.npz"),
                        X=Xva_all, Y=Yva_all, E=Eva_all.astype(str))
    model.save(os.path.join(args.outdir, "forecaster.keras"))

    # ---- Operator check: must be CONV_2D (MVP-accelerated), nothing exotic ---
    # Read the REAL operators out of the flatbuffer. An interpreter with XNNPACK
    # reports the delegate's operators rather than what is in the file, which is
    # easy to misdiagnose.
    from tensorflow.lite.python import schema_py_generated as schema
    mm = schema.Model.GetRootAsModel(tfl, 0)
    bnames = {v: k for k, v in vars(schema.BuiltinOperator).items() if isinstance(v, int)}
    ocodes = [mm.OperatorCodes(i).BuiltinCode() for i in range(mm.OperatorCodesLength())]
    sg = mm.Subgraphs(0)
    op_seq = [bnames.get(ocodes[sg.Operators(i).OpcodeIndex()], "?")
              for i in range(sg.OperatorsLength())]
    MVP_OK = {"CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED",
              "AVERAGE_POOL_2D", "MAX_POOL_2D", "ADD", "MUL", "RESHAPE"}
    print(f"Operators in model ({len(op_seq)}): {op_seq}")
    print(f"All within the MVP-accelerated set: {set(op_seq) <= MVP_OK}")

    interp = tf.lite.Interpreter(model_content=tfl)
    interp.allocate_tensors()

    inp_d = interp.get_input_details()[0]
    out_d = interp.get_output_details()[0]
    print(f"Input : shape={inp_d['shape']} dtype={inp_d['dtype'].__name__} "
          f"scale={inp_d['quantization'][0]:.10f} zp={inp_d['quantization'][1]}")
    print(f"Output: shape={out_d['shape']} dtype={out_d['dtype'].__name__} "
          f"scale={out_d['quantization'][0]:.10f} zp={out_d['quantization'][1]}")

    with open(os.path.join(args.outdir, "quant_params.txt"), "w") as f:
        f.write(f"IN_SCALE={inp_d['quantization'][0]:.12f}\n")
        f.write(f"IN_ZP={inp_d['quantization'][1]}\n")
        f.write(f"OUT_SCALE={out_d['quantization'][0]:.12f}\n")
        f.write(f"OUT_ZP={out_d['quantization'][1]}\n")
        f.write(f"WINDOW={WINDOW}\nHORIZON={HORIZON}\nN_CH={N_CH}\n")
        f.write(f"HR_CENTER={HR_CENTER}\nHR_SCALE={HR_SCALE}\n")
        f.write(f"SPO2_CENTER={SPO2_CENTER}\nSPO2_SCALE={SPO2_SCALE}\n")
        f.write(f"DR_CENTER={DR_CENTER}\nDR_SCALE={DR_SCALE}\n")
        f.write(f"WREL_SCALE={WREL_SCALE}\n")


if __name__ == "__main__":
    main()
