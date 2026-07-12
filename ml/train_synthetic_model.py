#!/usr/bin/env python3
"""
train_synthetic_model.py — runs ON YOUR LAPTOP, not the Pi.

Generates synthetic HC-SR04 distance windows for 3 movement classes,
trains a tiny MLP with tensorflow.keras, and exports it as
radar_model.tflite. The Pi never sees tensorflow — only hub/infer_radar.py
+ tflite_runtime touch the exported .tflite file.

Classes (must match hub/infer_radar.py's CLASS_LABELS order):
    0 = static      — baseline distance + small noise, no real trend
                       (also covers wind noise / sensor jitter)
    1 = approaching  — distance decreases fairly steadily across the window
    2 = passing      — distance drops toward the middle of the window, then
                       rises back up (something crossed the beam and left)

Input: 10 floats (raw centimeters, no scaling) — one HC-SR04 ping every
~200ms in reality on the Pi's 500ms poll loop... actually 10 pings at
GPIO_POLL_INTERVAL_MS=500 covers ~5s of real time; the "2 seconds" in the
original ask assumed a faster poll rate. Doesn't affect this script either
way — it just needs 10 sequential readings, whatever their real cadence.

Usage:
    pip install tensorflow numpy
    python3 train_synthetic_model.py
    # writes ../hub/radar_model.tflite (next to hub/main.c and
    # hub/infer_radar.py) — copy that one file to the Pi if you trained
    # on a laptop instead of on the Pi itself.
"""

import os
import numpy as np
import tensorflow as tf
from tensorflow import keras

WINDOW_LEN = 10
CLASS_LABELS = ["static", "approaching", "passing"]
SAMPLES_PER_CLASS = 2000
RANDOM_SEED = 42

# HC-SR04 practical range for this project — matches PROXIMITY_THRESHOLD_CM
# context in hub/main.c (breaches trigger classification well under this).
DIST_MIN_CM = 2.0
DIST_MAX_CM = 350.0
NOISE_STD_CM = 2.0  # per-reading sensor jitter, added to every class


def _clip(arr):
    return np.clip(arr, DIST_MIN_CM, DIST_MAX_CM)


def make_static_samples(n, rng):
    """Class 0: flat-ish baseline + noise. No consistent trend — this is
    what "nothing happening" (or wind rattling the mount) looks like."""
    baselines = rng.uniform(30.0, DIST_MAX_CM, size=n)
    noise = rng.normal(0.0, NOISE_STD_CM, size=(n, WINDOW_LEN))
    samples = baselines[:, None] + noise
    return _clip(samples)


def make_approach_samples(n, rng):
    """Class 1: starts far, ends close, roughly monotonic decrease."""
    start = rng.uniform(80.0, DIST_MAX_CM, size=n)
    end = rng.uniform(DIST_MIN_CM, 40.0, size=n)
    t = np.linspace(0.0, 1.0, WINDOW_LEN)
    trend = start[:, None] + (end - start)[:, None] * t[None, :]
    noise = rng.normal(0.0, NOISE_STD_CM, size=(n, WINDOW_LEN))
    return _clip(trend + noise)


def make_passing_samples(n, rng):
    """Class 2: starts far, dips low somewhere in the middle of the
    window, rises back toward far again — something crossed and kept
    going instead of stopping in front of the sensor."""
    start = rng.uniform(80.0, DIST_MAX_CM, size=n)
    end = rng.uniform(60.0, DIST_MAX_CM, size=n)
    dip = rng.uniform(DIST_MIN_CM, 50.0, size=n)
    dip_index = rng.integers(3, 7, size=n)  # dip lands roughly mid-window

    t = np.arange(WINDOW_LEN)
    samples = np.empty((n, WINDOW_LEN))
    for i in range(n):
        # Two linear segments: start -> dip, then dip -> end, joined at dip_index.
        di = dip_index[i]
        first_half = np.linspace(start[i], dip[i], di + 1)
        second_half = np.linspace(dip[i], end[i], WINDOW_LEN - di)
        samples[i] = np.concatenate([first_half, second_half[1:]])
    noise = rng.normal(0.0, NOISE_STD_CM, size=(n, WINDOW_LEN))
    return _clip(samples + noise)


def build_dataset():
    rng = np.random.default_rng(RANDOM_SEED)

    static = make_static_samples(SAMPLES_PER_CLASS, rng)
    approach = make_approach_samples(SAMPLES_PER_CLASS, rng)
    passing = make_passing_samples(SAMPLES_PER_CLASS, rng)

    X = np.concatenate([static, approach, passing], axis=0).astype("float32")
    y = np.concatenate([
        np.zeros(SAMPLES_PER_CLASS, dtype="int32"),
        np.ones(SAMPLES_PER_CLASS, dtype="int32"),
        np.full(SAMPLES_PER_CLASS, 2, dtype="int32"),
    ])

    # Shuffle so train/val split (below) isn't class-ordered.
    perm = rng.permutation(len(X))
    return X[perm], y[perm]


def build_model():
    """Deliberately tiny — this runs after conversion on a Pi 5 inside a
    5s classifier timeout (see CLASSIFIER_TIMEOUT_SEC in hub/main.c), so
    inference latency matters more than squeezing out accuracy."""
    model = keras.Sequential([
        keras.layers.Input(shape=(WINDOW_LEN,)),
        keras.layers.Dense(16, activation="relu"),
        keras.layers.Dense(8, activation="relu"),
        keras.layers.Dense(len(CLASS_LABELS), activation="softmax"),
    ])
    model.compile(
        optimizer="adam",
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    return model


def main():
    tf.random.set_seed(RANDOM_SEED)

    print("Generating synthetic training data...")
    X, y = build_dataset()
    split = int(len(X) * 0.8)
    X_train, X_val = X[:split], X[split:]
    y_train, y_val = y[:split], y[split:]
    print(f"  {len(X_train)} train / {len(X_val)} val samples, "
          f"{len(CLASS_LABELS)} classes: {CLASS_LABELS}")

    model = build_model()
    model.summary()

    print("Training...")
    model.fit(
        X_train, y_train,
        validation_data=(X_val, y_val),
        epochs=30,
        batch_size=32,
        verbose=2,
    )

    val_loss, val_acc = model.evaluate(X_val, y_val, verbose=0)
    print(f"Validation accuracy: {val_acc:.3f} (loss {val_loss:.3f})")

    print("Converting to TFLite...")
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    tflite_model = converter.convert()

    output_path = os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hub", "radar_model.tflite")
    )
    with open(output_path, "wb") as f:
        f.write(tflite_model)

    size_kb = len(tflite_model) / 1024
    print(f"Wrote {output_path} ({size_kb:.1f} KB)")
    print("If you trained this on your laptop instead of the Pi itself, "
          "copy that one file over to the Pi's hub/ directory — infer_radar.py "
          "loads it from next to itself, not from wherever it's invoked.")


if __name__ == "__main__":
    main()
