"""
Server to direct commands from the 3DS to the arduino for the Rockenbok
"""

import sys
import time
import serial
import socket
import struct
from pathlib import Path
import subprocess
from typing import List
import multiprocessing as mp
from multiprocessing import shared_memory as sm

#=========================CONFIGURATION SETTINGS=========================

#serial connection
SERIAL_PORT = "COM5"
BAUD_RATE = 250000
SEND_RATE_HZ = 30

#network connection
HOST = "0.0.0.0"
PORT = 5000         #for controls
VIDEO_PORT = 6000   #for streaming video

#video streaming
WEBCAM = "c922 Pro Stream Webcam"
BITRATE = "1200k"
FPS = "30"
MAX_ROLLING_BUFFER = 2 * 1024 * 1024  # 2 MiB

# H.264 start codes (Annex B). NAL units are delimited by one of these.
START3 = b"\x00\x00\x01"
START4 = b"\x00\x00\x00\x01"

#global variables
global stream_proc
#==================================================


#=========================HELPER FUNCTIONS=========================
def shutdown(ser):
    """
    Handles graceful shutdown of daemon process and servers
    """
    global stream_proc

    print("[CONTROL] Shutting down...")

    #shutdown serial connection
    ser.close()

    #shutdown daemon process
    stream_proc.terminate()
    time.sleep(0.25)
    stream_proc.close()

    print("[CONTROL] Done!")



def find_start_code(data: bytes, start: int = 0) -> int:
    """
    Finds the next occurence of an H.264 Annex-B start code in data
    """

    i = data.find(START4, start)
    j = data.find(START3, start)
    if i == -1:
        return j
    if j == -1:
        return i
    return min(i,j)

def start_code_len(data: bytes, idx: int) -> int:
    """
    Given a buffer and known start code index, return whether it is 3 or 4 bytes
    """

    if data.startswith(START4, idx):
        return 4
    else:
        return 3
    
def queue_complete_nal_units(rolling: bytes, nal_queue: List[bytes]) -> bytes:
    """
    Consumes as many COMPLETE NAL units as possible from rolling and appends them to nal_queue
    This includes the start codes in addition to payloads
    """

    #find the first start code in the buffer
    first = find_start_code(rolling, 0)
    if first == -1:
        # No start code at all, can't parse anything.
        return rolling
    
    #find the second start code to verify that there is a complete NAL unit
    second = find_start_code(rolling, first + start_code_len(rolling, first))
    if second == -1:
        return rolling
    
    #extract full NAL units for each pair of start codes
    start_i = first
    while True:
        start_next = find_start_code(rolling, start_i + start_code_len(rolling, start_i))
        if start_next == -1:
            #no further delimiter, so the NAL beginning at start_i is incomplete.
            break

        nal = rolling[start_i:start_next]
        if nal:
            nal_queue.append(nal)

        start_i = start_next

        #keep the remainder starting from last start code (incomplete NAL)
        return rolling[start_i:]
#==================================================


#=========================FFMPEG PIPELINE=========================
def ffmpeg_h264_stream_webcam(device_name: str) -> subprocess.Popen:
    vf = (
        "scale=400:240:force_original_aspect_ratio=decrease,"
        "pad=400:240:(400-iw)/2:(240-ih)/2,"
        "transpose=1"
    )

    cmd = [
        "ffmpeg",
        "-loglevel", "error",

        # Webcam input on Windows
        "-f", "dshow",
        "-i", f"video={device_name}",

        # Optional: force capture size/fps from the webcam before filtering
        "-video_size", "640x480",
        "-framerate", "30",

        "-vf", vf,
        "-r", FPS,

        "-c:v", "libx264",
        "-pix_fmt", "yuv420p",
        "-profile:v", "baseline",
        "-level:v", "3.0",
        "-tune", "zerolatency",
        "-g", str(int(FPS)),
        "-b:v", BITRATE,
        "-x264-params", "repeat-headers=1:scenecut=0:sync-lookahead=0:rc-lookahead=0:ref=1",
        "-bf", "0",
        "-preset", "ultrafast",
        "-f", "h264",
        "pipe:1",
    ]

    return subprocess.Popen(cmd, stdout=subprocess.PIPE)
#==================================================


#=========================VIDEO SERVER PROCESS=========================
def video_server_main():
    #create TCP server socket
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(1)

    print(f"[VIDEO] Listening on 0.0.0.0:{VIDEO_PORT}")
    print(f"[VIDEO] Waiting for client...")

    conn, addr = srv.accept()
    print(f"[VIDEO] Client connected from {addr}")

    #conduct handshake, like a true gentleman
    hello = conn.recv(5) #5 bytes, expects 'YOINK'
    if hello != b"YOINK":
        print(f"[VIDEO] Bad handshake: got {hello!r}, expected b'YOINK'")
        conn.close()
        srv.close()
        return
    
    conn.sendall(b"YEET\n")
    print("[VIDEO] Handshake OK (YOINK <-> YEET)")

    #start ffmpeg
    proc = ffmpeg_h264_stream_webcam(WEBCAM)

    assert proc.stdout is not None
 
    rolling = b""                   #rolling buffer for ffmpeg bytes before they are split into NAL units
    nal_queue: List[bytes] = []     #queue of complete NAL units ready to send

    #stream loop
    try:
        while True:
            #ensure at least one complete NAL is available
            while not nal_queue:
                chunk = proc.stdout.read(8192)
                if not chunk:
                    #ffmpeg ended; send end-of-stream marker
                    conn.sendall(struct.pack(">I", 0))
                    print("[VIDEO] End-of-stream sent (length=0).")
                    return
                
                rolling += chunk
                rolling = queue_complete_nal_units(rolling, nal_queue)

                #keep memory bounded if start codes have been missing for a while
                if len(rolling) > MAX_ROLLING_BUFFER:
                    rolling = rolling[-MAX_ROLLING_BUFFER:]

            nal = nal_queue.pop(0)

            #send length-prefixed NAL (big-endian u32), then send bytes
            try:
                conn.sendall(struct.pack(">I", len(nal)))
                conn.sendall(nal)
            except OSError as e:
                print(f"[VIDEO] client connection closed: {e}"
                      break)
                
    finally:
        #Cleanup
        print("[VIDEO] Initiating cleanup...")
        conn.close()
        srv.close()
        proc.kill()
        print("[VIDEO] Clean shutdown.")
#==================================================

def main():
    global stream_proc

    # Open serial connection to Arduino
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0)
    except serial.SerialException as e:
        print(f"[CONTROL] Failed to open serial port {SERIAL_PORT}: {e}")
        sys.exit(1)

    # Give the Arduino a moment.
    # Many Arduino boards reset when the serial port opens.
    time.sleep(2.0)

    print(f"[CONTROL] Connected to {SERIAL_PORT} at {BAUD_RATE} baud.")

    #open websocket and connect to 3ds
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(1)
        print(f"[CONTROL] Listening on {HOST}:{PORT} ...")

        conn, addr = s.accept()
        with conn:
            print(f"[CONTROL] Client connected from {addr}")

            data = conn.recv(1024)
            print(f"[CONTROL] Handshake recv: {data!r}")

            conn.sendall(b"WHORE\n")

            running = True
            last_command_sent = None

            #now start video streaming process
            stream_proc = mp.Process(target=video_server_main,
                             daemon=True,
                             name="VideoProc")
            stream_proc.start()

            try:
                while running:
                    #receive command
                    command = conn.recv(2)

                    #3ds closed the connection
                    if not command:
                        print("[CONTROL] 3DS disconnected.")
                        break

                    if command != last_command_sent:
                        ser.write(command)
                        last_command_sent = command
                        print(f"[CONTROL] Sent command: {command}")
                    else:
                        ser.write(command)

                    #display serial output from arduino (if any)
                    try:
                        waiting = ser.in_waiting
                        if waiting > 0:
                            incoming = ser.read(waiting).decode(errors="ignore")
                            if incoming.strip():
                                print(incoming, end="")
                    except Exception:
                        # Ignore serial decode/read hiccups
                        pass

            finally:
                try:
                    ser.close()
                except Exception:
                    pass

                print("\n[CONTROL] Disconnected.")


if __name__ == "__main__":
    main()