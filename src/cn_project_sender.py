# cn_project_sender.py
# Termux / Python3 script to send a file to the C++ receiver that expects:
# [4-byte big-endian length][payload bytes]
# Optionally waits for a 1-byte ACK.

import socket
import os
import sys

# === CONFIGURE THESE ===
SERVER_IP = "192.168.44.107" # Laptop's Bluetooth PAN IP
PORT = 5001
FILE_PATH = "/data/data/com.termux/files/home/storage/downloads/duckyfile.dd"

SEND_ACK_EXPECTED = True # Set False if receiver started with --no-ack
ACK_TIMEOUT_SECONDS = 5

# -------------------------
def send_file(path, server_ip, port, expect_ack=True):
    if not os.path.isfile(path):
        print(f"[ERROR] File not found: {path}")
        return 1

    file_size = os.path.getsize(path)
    if file_size == 0:
        print("[WARN] File is empty, sending zero bytes")

    print(f"[INFO] Connecting to {server_ip}:{port} ...")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(10)
            s.connect((server_ip, port))

            # Send 4-byte big-endian length
            length_prefix = file_size.to_bytes(4, byteorder='big')
            s.sendall(length_prefix)
            print(f"[INFO] Sent length prefix: {file_size} bytes")

            # Send file in chunks
            sent = 0
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    s.sendall(chunk)
                    sent += len(chunk)

            print(f"[OK] Sent file bytes: {sent}")

            # Wait for ACK
            if expect_ack:
                s.settimeout(ACK_TIMEOUT_SECONDS)
                try:
                    ack = s.recv(1)
                    if ack == b"\x01":
                        print("[OK] ACK received from server.")
                    else:
                        print(f"[WARN] Unexpected ACK: {ack!r}")
                except socket.timeout:
                    print("[WARN] No ACK received (timeout).")
                except Exception as e:
                    print("[WARN] Error waiting for ACK:", e)

    except Exception as e:
        print("[ERROR] Network connection failed:", e)
        return 2

    return 0

if __name__ == "__main__":
    sys.exit(send_file(FILE_PATH, SERVER_IP, PORT, SEND_ACK_EXPECTED))