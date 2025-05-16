# server.py
import socket
import time
import struct
import threading
from collections import deque, defaultdict
import re

# Configuration
HOST = '127.0.0.1'
PORT = 60001
WINDOW_SIZE = 30           # number of samples
WINDOW_EXPIRY = 300        # seconds: expire data older than this
SLOPE_THRESHOLD = 0.5      # slope threshold to trigger valve

# Pre-compute sums for fixed-interval indices
SUM_I = sum(range(WINDOW_SIZE))
SUM_I2 = sum(i * i for i in range(WINDOW_SIZE))

# Data store: node_id -> deque of (timestamp, value)
data_windows = defaultdict(lambda: deque(maxlen=WINDOW_SIZE))
lock = threading.Lock()

# Regex to parse lines like: "PROCESS : Server got ID=3, value=42"
LINE_RE = re.compile(r"ID=(\d+),\s*value=(\d+)")


def compute_slope_fixed(values):
    n = len(values)
    sum_v = sum(values)
    sum_iv = sum(i * values[i] for i in range(n))
    num = n * sum_iv - SUM_I * sum_v
    den = n * SUM_I2 - SUM_I * SUM_I
    return (num / den) if den != 0 else 0.0


def handle_reading(node_id, value, sock):
    now = time.time()
    with lock:
        dq = data_windows[node_id]
        # Remove expired data
        while dq and now - dq[0][0] > WINDOW_EXPIRY:
            dq.popleft()
        # Append new reading (oldest auto removed when maxlen reached)
        dq.append((now, value))
        # Compute slope once we have exactly WINDOW_SIZE points
        if len(dq) == WINDOW_SIZE:
            values = [v for (_, v) in dq]
            slope = compute_slope_fixed(values)
            print(f"Node {node_id}: slope={slope:.3f} based on {len(dq)} pts")
            if slope > SLOPE_THRESHOLD:
                print(f"--> Triggering OPEN_VALVE for node {node_id}")
                # Pack message: type=3 (open valve), node_id, code=1

                cmd = f"3 {node_id} 1\n"
                sock.send(cmd.encode('ascii'))
                print(f"→ Sent ASCII cmd: {cmd.strip()}")
                dq.clear()  # clear after triggering


def serial_listener(sock):
    buffer = b''
    while True:
        try:
            chunk = sock.recv(1024)
            if not chunk:
                print("Connection closed by Cooja, exiting listener.")
                break
            buffer += chunk
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                text = line.decode('ascii', errors='ignore').strip()
                m = LINE_RE.search(text)
                if m:
                    node_id = int(m.group(1))
                    value = int(m.group(2))
                    handle_reading(node_id, value, sock)
        except Exception as e:
            print("Error in serial listener:", e)
            break


def main():
    # Connect to Cooja's serial socket
    while True:
        try:
            print(f"Connecting to {HOST}:{PORT}...")
            sock = socket.create_connection((HOST, PORT))
            print("Connected to Cooja serial port.")
            # Flush any backlog from Cooja
            sock.settimeout(0.1)
            try:
                while True:
                    sock.recv(1024)
            except socket.timeout:
                pass
            sock.settimeout(None)
            print("Buffer flushed; starting live processing.")
            break
        except ConnectionRefusedError:
            print("Connection refused, retrying in 2 seconds...")
            time.sleep(2)

    # Start listener thread
    listener = threading.Thread(target=serial_listener, args=(sock,), daemon=True)
    listener.start()

    # Keep main thread alive
    try:
        while listener.is_alive():
            time.sleep(1)
    except KeyboardInterrupt:
        print("Shutting down server.")
        sock.close()


if __name__ == '__main__':
    main()
