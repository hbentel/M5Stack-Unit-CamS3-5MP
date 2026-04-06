import urllib.request
import threading
import time
import argparse
import sys

def mjpeg_client_task(ip, client_id, stop_event):
    url = f"http://{ip}:81/stream"
    print(f"[Client {client_id}] Connecting to {url}...")
    try:
        with urllib.request.urlopen(url, timeout=5) as response:
            if response.status != 200:
                print(f"[Client {client_id}] Failed to connect: {response.status}")
                return
            
            print(f"[Client {client_id}] Connected. Receiving frames...")
            frame_count = 0
            start_time = time.time()
            
            while not stop_event.is_set():
                # Read in chunks
                chunk = response.read(2048)
                if not chunk:
                    break
                
                # Count frames by looking for JPEG Start of Image marker
                frame_count += chunk.count(b'\xff\xd8')
                
                # Report FPS every 50 frames
                if frame_count > 0 and frame_count % 50 == 0:
                    elapsed = time.time() - start_time
                    if elapsed > 0:
                        fps = frame_count / elapsed
                        print(f"[Client {client_id}] Received {frame_count} frames (~{fps:.1f} FPS)")
                        # Reset counter to keep reporting fresh stats
                        frame_count = 0
                        start_time = time.time()
                    
    except Exception as e:
        print(f"[Client {client_id}] Error: {e}")

def main():
    parser = argparse.ArgumentParser(description="Test concurrent MJPEG stream clients (Standard Lib version).")
    parser.add_argument("ip", help="IP address of the ESP32 camera")
    parser.add_argument("--clients", type=int, default=2, help="Number of concurrent clients (default: 2)")
    parser.add_argument("--duration", type=int, default=30, help="Test duration in seconds (default: 30)")
    args = parser.parse_args()

    stop_event = threading.Event()
    threads = []
    
    print(f"Starting concurrency test with {args.clients} clients...")
    for i in range(args.clients):
        t = threading.Thread(target=mjpeg_client_task, args=(args.ip, i, stop_event))
        t.daemon = True # Ensure threads exit if main process exits
        t.start()
        threads.append(t)
        time.sleep(0.5)

    try:
        time.sleep(args.duration)
    except KeyboardInterrupt:
        print("\nTest interrupted by user.")
    finally:
        print("Stopping test...")
        stop_event.set()
        # Threads are daemonized so they won't hang the process if read is blocked
        print("Test complete.")

if __name__ == "__main__":
    main()
