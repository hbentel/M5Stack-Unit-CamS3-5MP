#!/usr/bin/env python3
"""
Stress test for the UnitCamS3 MJPEG broadcaster.
Opens N simultaneous streams, counts frames, measures per-client FPS,
detects stalls and errors, and prints a live summary table.

Usage:
    python3 test_stream_stress.py [--clients N] [--duration S] [--url URL]

Defaults: 5 clients, 60 seconds, http://192.168.50.44:81/stream
"""

import argparse
import threading
import time
import requests
import json
import urllib.request
from dataclasses import dataclass, field
from typing import List

STATS_URL = "http://192.168.50.44/stats"
HEALTH_URL = "http://192.168.50.44/health"
BOUNDARY = b"--frame"
SOI = b"\xff\xd8"
EOI = b"\xff\xd9"


@dataclass
class ClientResult:
    client_id: int
    frames: int = 0
    bytes_received: int = 0
    bad_jpeg: int = 0
    errors: int = 0
    start_time: float = 0.0
    end_time: float = 0.0
    last_frame_time: float = 0.0
    max_gap_s: float = 0.0
    stopped: bool = False
    error_msg: str = ""


def stream_client(client_id: int, url: str, duration: float, result: ClientResult, stop_event: threading.Event):
    result.start_time = time.monotonic()
    result.last_frame_time = result.start_time

    try:
        resp = requests.get(url, stream=True, timeout=15)
        resp.raise_for_status()

        buf = b""
        for chunk in resp.iter_content(chunk_size=4096):
            if stop_event.is_set():
                break

            buf += chunk

            # Parse multipart MJPEG: find complete JPEG frames between boundaries
            while True:
                soi = buf.find(SOI)
                if soi == -1:
                    break
                eoi = buf.find(EOI, soi + 2)
                if eoi == -1:
                    break

                jpeg = buf[soi:eoi + 2]
                buf = buf[eoi + 2:]

                now = time.monotonic()
                gap = now - result.last_frame_time
                if gap > result.max_gap_s:
                    result.max_gap_s = gap
                result.last_frame_time = now

                # Validate JPEG
                if jpeg[:2] == SOI and jpeg[-2:] == EOI:
                    result.frames += 1
                    result.bytes_received += len(jpeg)
                else:
                    result.bad_jpeg += 1

            if time.monotonic() - result.start_time >= duration:
                break

    except Exception as e:
        result.error_msg = str(e)
        result.errors += 1

    result.end_time = time.monotonic()
    result.stopped = True


def get_device_stats():
    try:
        with urllib.request.urlopen(STATS_URL, timeout=3) as r:
            return json.loads(r.read())
    except Exception:
        return None


def fps_str(result: ClientResult) -> str:
    elapsed = (result.end_time or time.monotonic()) - result.start_time
    if elapsed < 0.1:
        return "  ---"
    return f"{result.frames / elapsed:5.2f}"


def print_table(results: List[ClientResult], elapsed: float):
    print(f"\n{'─'*72}")
    print(f"  t={elapsed:5.1f}s   {'Client':<8} {'Frames':>7} {'FPS':>6} {'KB':>7} {'BadJPEG':>8} {'MaxGap':>7} {'Status'}")
    print(f"{'─'*72}")
    for r in results:
        status = "ERROR: " + r.error_msg[:20] if r.error_msg else ("done" if r.stopped else "streaming")
        kb = r.bytes_received // 1024
        gap = f"{r.max_gap_s:.2f}s" if r.max_gap_s > 0 else "  ---"
        print(f"  {'client-'+str(r.client_id):<8} {r.frames:>7} {fps_str(r):>6} {kb:>7} {r.bad_jpeg:>8} {gap:>7}  {status}")
    print(f"{'─'*72}")


def print_device_stats(stats):
    if not stats:
        print("  [device stats unavailable]")
        return
    cam = stats.get("camera", {})
    wifi = stats.get("wifi", {})
    mem = stats.get("memory", {})
    print(f"  Device: fps={cam.get('fps', '?'):.1f}  no_soi={cam.get('no_soi', '?')}  "
          f"no_eoi={cam.get('no_eoi', '?')}  drops={cam.get('drops_no_buf', '?')}  "
          f"streams={cam.get('active_streams', '?')}  "
          f"rssi={wifi.get('rssi', '?')}dBm  "
          f"disconnects={wifi.get('disconnects', '?')}  "
          f"psram_free={mem.get('psram_free', 0)//1024}KB")


def main():
    parser = argparse.ArgumentParser(description="UnitCamS3 MJPEG stream stress test")
    parser.add_argument("--clients", type=int, default=5, help="Number of simultaneous streams (default: 5)")
    parser.add_argument("--duration", type=float, default=60.0, help="Test duration in seconds (default: 60)")
    parser.add_argument("--url", default="http://192.168.50.44:81/stream", help="Stream URL")
    parser.add_argument("--interval", type=float, default=5.0, help="Stats print interval (default: 5s)")
    args = parser.parse_args()

    print(f"\nUnitCamS3 Stream Stress Test")
    print(f"  Clients  : {args.clients}")
    print(f"  Duration : {args.duration}s")
    print(f"  URL      : {args.url}")
    print(f"  Interval : {args.interval}s\n")

    # Print baseline device stats
    print("Baseline device stats:")
    print_device_stats(get_device_stats())
    print()

    results = [ClientResult(client_id=i) for i in range(args.clients)]
    stop_event = threading.Event()
    threads = []

    for i, result in enumerate(results):
        t = threading.Thread(
            target=stream_client,
            args=(i, args.url, args.duration, result, stop_event),
            daemon=True,
            name=f"client-{i}"
        )
        threads.append(t)

    # Stagger starts slightly to avoid hammering the device simultaneously
    print("Starting clients...")
    for t in threads:
        t.start()
        time.sleep(0.2)

    start = time.monotonic()
    try:
        while True:
            time.sleep(args.interval)
            elapsed = time.monotonic() - start
            print_table(results, elapsed)
            print_device_stats(get_device_stats())

            if all(r.stopped for r in results) or elapsed >= args.duration + 5:
                break

    except KeyboardInterrupt:
        print("\nInterrupted — stopping clients...")
        stop_event.set()

    stop_event.set()
    for t in threads:
        t.join(timeout=5)

    elapsed = time.monotonic() - start
    print(f"\n{'='*72}")
    print(f"FINAL RESULTS  ({elapsed:.1f}s)")
    print(f"{'='*72}")
    print_table(results, elapsed)

    # Final device stats
    print("Final device stats:")
    print_device_stats(get_device_stats())

    # Summary
    total_frames = sum(r.frames for r in results)
    total_errors = sum(r.errors for r in results)
    total_bad = sum(r.bad_jpeg for r in results)
    avg_fps = sum(r.frames / max((r.end_time or time.monotonic()) - r.start_time, 0.1) for r in results) / len(results)
    max_gap = max(r.max_gap_s for r in results)

    print(f"\nSummary:")
    print(f"  Total frames received : {total_frames}")
    print(f"  Avg FPS per client    : {avg_fps:.2f}")
    print(f"  Total errors          : {total_errors}")
    print(f"  Bad JPEGs             : {total_bad}")
    print(f"  Max frame gap (any)   : {max_gap:.2f}s")
    print(f"  {'PASS' if total_errors == 0 and total_bad == 0 else 'FAIL'}")
    print()


if __name__ == "__main__":
    main()
