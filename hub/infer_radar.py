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

Two modes:

  SERVER MODE (default — no args). hub/main.c forks+execs this ONCE at
  startup (see radar_start_locked() in main.c) and keeps its stdin/stdout
  pipes open for the life of the hub process, instead of popen()'ing a
  fresh interpreter on every single proximity breach — loading the model
  and standing up the XNNPACK delegate is the slow part, so paying that
  cost once instead of per-breach is the whole point. Reads one line of
  JSON per request from stdin, e.g.:

      [120.0, 115.0, 108.0, 95.0, 80.0, 60.0, 45.0, 30.0, 20.0, 12.0]

  and writes exactly one line back to stdout: the predicted label
  ("static", "approaching", "passing", or "unknown" on any per-request
  failure — a bad request doesn't crash the server, so hub doesn't need
  to respawn it just because one line of input was malformed). All
  diagnostics go to stderr, never stdout, since hub/main.c reads stdout
  one fgets() line per request and a stray extra line would desync the
  protocol.

  ONE-SHOT MODE (CLI args, for manual testing only — hub/main.c no
  longer uses this):

      ./infer_radar.py <d0> <d1> ... <d9>

  10 positional args, oldest reading first, raw centimeters, no scaling.
  Prints one label to stdout and exits. Loads its own interpreter each
  time, which is exactly the per-call cost server mode avoids.

Manual test (one-shot):
    ./infer_radar.py 120.0 115.0 108.0 95.0 80.0 60.0 45.0 30.0 20.0 12.0

Manual test (server mode):
    ./infer_radar.py
    [120.0, 115.0, 108.0, 95.0, 80.0, 60.0, 45.0, 30.0, 20.0, 12.0]
    (type/paste a JSON array + Enter, get a label back, repeat, Ctrl+D to quit)
"""

import json
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


def serve():
    """Server mode: loads the interpreter ONCE, then services one
    classification per line of stdin (a JSON array of WINDOW_LEN floats),
    writing the predicted label to stdout per line. Runs until stdin
    closes (hub exiting, or hub killing this process to restart it — see
    radar_stop_locked() in main.c). A bad request logs to stderr and
    answers "unknown" rather than crashing the whole server, since one
    malformed line shouldn't take down a process hub expects to stay up
    for its entire run."""
    try:
        interpreter = load_interpreter()
    except Exception as e:
        # No model file, no tflite_runtime, etc — nothing to serve with.
        # Exit non-zero so hub's radar_classify() sees the pipe close
        # (read() -> EOF) on its very first request and logs accordingly,
        # rather than hanging until CLASSIFIER_TIMEOUT_SEC every time.
        print(f"infer_radar: failed to load interpreter, exiting: {e}", file=sys.stderr)
        return 1

    print("infer_radar: model loaded, ready for requests", file=sys.stderr)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            distances = json.loads(line)
            if not isinstance(distances, list):
                raise ValueError("expected a JSON array of floats")
            label = predict_movement([float(d) for d in distances], interpreter=interpreter)
        except Exception as e:
            print(f"infer_radar: request failed ({line!r}): {e}", file=sys.stderr)
            label = "unknown"

        print(label, flush=True)

    return 0


def main():
    if len(sys.argv) == 1:
        return serve()

    if len(sys.argv) != WINDOW_LEN + 1:
        print(f"usage: {sys.argv[0]} <d0> <d1> ... <d9>  ({WINDOW_LEN} distances in cm)"
              f"\n       {sys.argv[0]}  (no args -> server mode, see module docstring)",
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
