/*
 * capture_snapshot.c — standalone QNX camera snapshot tool.
 *
 * Captures exactly ONE frame from the Pi camera via QNX's camera_api and
 * writes it to /tmp/latest_snapshot.raw, then exits. This is a separate
 * program from hub/main.c on purpose — it does not touch the HTTP server,
 * SQLite, GPIO polling, or LED thread already running there. hub/main.c's
 * trigger_camera_and_classify() shells out to this binary (via system())
 * right before it runs the OpenCV classifier, so the classifier always has
 * a fresh frame waiting at OUTPUT_PATH.
 *
 * ----------------------------------------------------------------------
 * API CONFIDENCE NOTE — READ BEFORE COMPILING ON THE PI
 * ----------------------------------------------------------------------
 * I don't have the real QNX 8.0 camera headers available to check this
 * against, so the parts below marked BEST-EFFORT are my reconstruction of
 * the camera_api shape (camera_take_photo()'s callback signature and the
 * camera_buffer_t/frametype union field names) — they may not match your
 * actual /usr/include/camera/camera_api.h exactly. Before trusting this
 * to compile as-is on the Pi, run:
 *
 *     grep -B2 -A20 "camera_buffer_t" /usr/include/camera/camera_api.h
 *     grep -B2 -A15 "camera_take_photo" /usr/include/camera/camera_api.h
 *
 * and adjust the two marked sections (write_frame_to_disk's field access,
 * and the camera_take_photo() call's callback signature/arg order) to
 * match what's actually there. Everything else — program structure, file
 * writing, exit codes — should need no changes.
 *
 * Build (on the Pi):
 *     qcc -Vgcc_ntoaarch64le -Wall -O2 -o capture_snapshot capture_snapshot.c -lcamera
 * If qcc isn't on PATH but gcc is aliased to the QNX toolchain instead:
 *     gcc -Wall -O2 -o capture_snapshot capture_snapshot.c -lcamera
 *
 * Run standalone (for testing before wiring it into hub/main.c):
 *     ./capture_snapshot
 *     echo $?                       # 0 = wrote a frame, 1 = failed
 *     ls -la /tmp/latest_snapshot.raw
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <camera/camera_api.h>

#define OUTPUT_PATH "/tmp/latest_snapshot.raw"

static int g_frame_written = 0;
static int g_capture_failed = 0;

/* Writes one frame's raw bytes to OUTPUT_PATH, overwriting any previous
 * snapshot so the Python/OpenCV side always reads the latest one. */
static void write_frame_to_disk(camera_buffer_t *buf) {
    if (!buf) {
        fprintf(stderr, "capture_snapshot: NULL buffer in completion callback\n");
        g_capture_failed = 1;
        return;
    }

    /* ---- BEST-EFFORT: verify against your real camera_api.h ---- */
    const void *data = NULL;
    size_t size = 0;

    if (buf->frametype == CAMERA_FRAMETYPE_JPEG) {
        data = buf->framedesc.jpeg.buf;
        size = buf->framedesc.jpeg.bufsize;
    } else {
        /* Raw/uncompressed frame (e.g. NV12) — adjust member names if
         * your header calls this union arm something other than "raw". */
        data = buf->framedesc.raw.buf;
        size = buf->framedesc.raw.bufsize;
    }
    /* ---- END BEST-EFFORT SECTION ---- */

    if (!data || size == 0) {
        fprintf(stderr, "capture_snapshot: empty frame buffer, nothing written\n");
        g_capture_failed = 1;
        return;
    }

    FILE *f = fopen(OUTPUT_PATH, "wb");
    if (!f) {
        fprintf(stderr, "capture_snapshot: fopen(%s) failed: %s\n", OUTPUT_PATH, strerror(errno));
        g_capture_failed = 1;
        return;
    }

    size_t written = fwrite(data, 1, size, f);
    fclose(f);

    if (written != size) {
        fprintf(stderr, "capture_snapshot: short write (%zu/%zu bytes)\n", written, size);
        g_capture_failed = 1;
        return;
    }

    fprintf(stderr, "capture_snapshot: wrote %zu bytes to %s\n", size, OUTPUT_PATH);
    g_frame_written = 1;
}

/* Final-frame callback from camera_take_photo(). Shutter/raw/postview
 * callbacks are left NULL in main() below — we only need the finished
 * image, not the intermediate capture stages. */
static void on_photo_complete(camera_handle_t handle, camera_buffer_t *buf, void *arg) {
    (void)handle;
    (void)arg;
    write_frame_to_disk(buf);
}

int main(void) {
    camera_handle_t cam_handle;
    camera_error_t err;

    err = camera_open(CAMERA_UNIT_REAR, CAMERA_MODE_RW, &cam_handle);
    if (err != CAMERA_EOK) {
        fprintf(stderr, "capture_snapshot: camera_open failed (err=%d). "
                        "Is the camera resource manager (io-usb/cam) running?\n", (int)err);
        return 1;
    }

    /* ---- BEST-EFFORT: verify this call's signature against your header ---- */
    /* Single blocking photo capture: shutter/raw/postview callbacks NULL,
     * only the completion callback set, wait=true blocks inside this call
     * until the photo is done so main() can just exit right after —
     * no event loop needed for a one-shot snapshot tool. */
    err = camera_take_photo(cam_handle, NULL, NULL, NULL, on_photo_complete, NULL, true);
    /* ---- END BEST-EFFORT SECTION ---- */

    if (err != CAMERA_EOK) {
        fprintf(stderr, "capture_snapshot: camera_take_photo failed (err=%d)\n", (int)err);
        camera_close(cam_handle);
        return 1;
    }

    camera_close(cam_handle);

    if (!g_frame_written || g_capture_failed) {
        fprintf(stderr, "capture_snapshot: no usable frame was written — treat this run as a failure\n");
        return 1;
    }

    return 0;
}
