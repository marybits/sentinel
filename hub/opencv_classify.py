#!/usr/bin/env python3
"""
Sentinel — opencv_classify.py
-------------------------------------------------------------------------
Invoked by hub/main.c (via popen) whenever the HC-SR04 detects an object
within 30cm. Captures a single frame from the Pi Camera and classifies it
using OpenCV (satisfies the QNX hackathon requirement: open-source AI from
oss.qnx.com — the QNX package repo carries OpenCV).

Contract with main.c:
    - Print exactly ONE line to stdout: "person", "object", or "none".
    - Any diagnostic/debug output goes to stderr, not stdout.
    - Exit quickly (main.c enforces a 5s timeout around this process).

Classification approach (intentionally simple for hackathon reliability —
no external model downloads required):
    1. Try OpenCV's built-in HOG + default people detector to look for a
       person silhouette. This ships inside opencv-python, no extra files.
    2. If no person is found, fall back to a basic motion/contour check
       against a quick background sample to decide "object" vs "none".

NOTE: a true "animal vs. object" distinction would need a trained model
(e.g. a small MobileNet-SSD) — intentionally left as a follow-up rather
than faking a classifier we don't actually have.

Install (on the Pi, via apk):
    apk add opencv opencv-python py3-opencv   # verify actual package
                                                # name(s) in your QNX apk
                                                # repo — names vary by
                                                # image/version.
    or: pip3 install opencv-python-headless
"""

import sys

def log(msg):
    print(msg, file=sys.stderr)

def classify():
    try:
        import cv2
    except ImportError:
        log("opencv (cv2) not importable — install via apk/pip. Falling back to 'unknown'.")
        print("unknown")
        return

    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        log("Could not open camera at index 0 — check Pi Camera is enabled "
            "and /dev/video0 (or equivalent) exists.")
        print("unknown")
        return

    # Let the sensor settle for a couple of frames (first frame off some
    # camera stacks is dark/garbage).
    frame = None
    for _ in range(3):
        ok, frame = cap.read()
        if not ok:
            frame = None
    cap.release()

    if frame is None:
        log("Failed to capture a frame.")
        print("unknown")
        return

    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

    # --- Step 1: person detection via built-in HOG descriptor ---
    hog = cv2.HOGDescriptor()
    hog.setSVMDetector(cv2.HOGDescriptor_getDefaultPeopleDetector())
    boxes, weights = hog.detectMultiScale(frame, winStride=(8, 8))
    if len(boxes) > 0:
        log(f"HOG person detector found {len(boxes)} candidate(s), max weight={max(weights):.2f}")
        print("person")
        return

    # --- Step 2: fall back to a simple "is there something here at all"
    # check via edge density. Not a real object classifier — just enough
    # to distinguish "empty frame" from "something in frame" for the demo.
    edges = cv2.Canny(gray, 80, 160)
    edge_ratio = (edges > 0).mean()
    log(f"edge_ratio={edge_ratio:.4f}")

    if edge_ratio > 0.02:
        print("object")
    else:
        print("none")

if __name__ == "__main__":
    classify()
