#!/usr/bin/env python3
"""
infer_radar.py — runs ON THE PI. Classifies a movement pattern from the
last 10 HC-SR04 distance readings (~2s of pings) using a tiny TFLite MLP.

Uses ONLY tflite_runtime.interpreter — never imports tensorflow. QNX can't
easily run full TensorFlow on-device, but tflite_runtime is confirmed
working here (`from tflite_runtime.interpreter import Interpreter`).

Model: radar_model.tflite, trained on your laptop by
../ml/train_synthetic_model.py and copied into this directory (hub/).
Class order MUST match training: 0=static, 1=approaching, 2=passing.

Invocation (hub/main.c calls this via popen(), same pattern the old
opencv_classify.py used — see trigger_radar_classification()):

    ./infer_radar.py <d0> <d1> ... <d9>

10 positional args, oldest reading first, raw centimeters, no scaling.
Prints exactly one line to stdout: the predicted label ("static",
"approaching", "passing", or "unknown" on any failure) — nothing else on
stdout, since hub/main.c's run_classifier_with_timeout() reads a single
fgets() line. All diagnostics go to stderr instead.

Manual test:
    ./infer_radar.py 120.0 115.0 108.0 95.0 80.0 60.0 45.0 30.0 20.0 12.0
"""

import os
import sys

WINDOW_LEN = 10
CLASS_LABELS = ["static", "approaching", "passing"]
MODEL_FILENAME = "radar_model.tflite"


def load_interpreter():
    """Loads radar_model.tflite from next to this script (not the cwd —
    hub/main.c may invoke this from a different working directory)."""
    from tflite_runtime.interpreter import Interpreter

    model_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), MODEL_FILENAME)
    if not os.path.isfile(model_path):
        raise FileNotFoundError(
            f"{MODEL_FILENAME} not found next to infer_radar.py at {model_path} — "
            f"run ml/train_synthetic_model.py on your laptop and copy the .tflite here."
        )

    interpreter = Interpreter(model_path=model_path)
    interpreter.allocate_tensors()
    return interpreter


def predict_movement(distance_array, interpreter=None):
    """Takes a list of WINDOW_LEN floats (oldest first), runs inference,
    returns the predicted class label as a string. Loads its own
    interpreter if one isn't passed in (CLI usage is one-shot per process,
    so there's no benefit to caching it across calls here)."""
    if len(distance_array) != WINDOW_LEN:
        raise ValueError(f"expected {WINDOW_LEN} readings, got {len(distance_array)}")

    if interpreter is None:
        interpreter = load_interpreter()

    import numpy as np

    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()

    x = np.array([distance_array], dtype=input_details[0]["dtype"])
    interpreter.set_tensor(input_details[0]["index"], x)
    interpreter.invoke()

    output = interpreter.get_tensor(output_details[0]["index"])
    class_index = int(np.argmax(output[0]))

    if class_index < 0 or class_index >= len(CLASS_LABELS):
        return "unknown"
    return CLASS_LABELS[class_index]


def main():
    if len(sys.argv) != WINDOW_LEN + 1:
        print(f"usage: {sys.argv[0]} <d0> <d1> ... <d9>  ({WINDOW_LEN} distances in cm)",
              file=sys.stderr)
        print("unknown")
        return 1

    try:
        distances = [float(a) for a in sys.argv[1:]]
    except ValueError as e:
        print(f"infer_radar: couldn't parse distances: {e}", file=sys.stderr)
        print("unknown")
        return 1

    try:
        label = predict_movement(distances)
    except Exception as e:
        # Anything from a missing model file to a tflite_runtime import
        # error lands here — never let hub's popen() hang waiting on a
        # traceback with no final line.
        print(f"infer_radar: inference failed: {e}", file=sys.stderr)
        print("unknown")
        return 1

    print(label)
    return 0


if __name__ == "__main__":
    sys.exit(main())
