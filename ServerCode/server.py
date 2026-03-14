"""
Server to direct commands from the 3DS to the arduino for the Rockenbok
"""

import sys
import time
import serial
import socket
import struct
import subprocess
from typing import List
import multiprocessing as mp
import ctypes

#=========================CONFIGURATION SETTINGS=========================

#user settings
STREAM_VIDEO = True     #change this to false if running the verion without video streaming

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
BITRATE = "500k"
FPS = "20"
MAX_ROLLING_BUFFER = 2 * 1024 * 1024  # 2 MiB

# H.264 start codes (Annex B). NAL units are delimited by one of these.
START3 = b"\x00\x00\x01"
START4 = b"\x00\x00\x00\x01"

#shared video-process state values
VIDEO_STATE_STOPPED   = 0   #video process is not running
VIDEO_STATE_STARTING  = 1   #video process has been spawned but is not ready yet
VIDEO_STATE_WAITING   = 2   #video server socket is listening and waiting for client
VIDEO_STATE_STREAMING = 3   #video client connected and stream loop active
VIDEO_STATE_STOPPING  = 4   #video process is shutting down
VIDEO_STATE_ERROR     = 5   #video process hit an error

#global variables
global stream_proc
stream_proc = None
global ser
ser = None
global video_state
video_state = None
#==================================================


#=========================HELPER FUNCTIONS=========================
def create_video_shared_state():
    """
    Create the shared memory object used to communicate the video process state
    between the control process and the video process.
    """
    return mp.Value(ctypes.c_int, VIDEO_STATE_STOPPED, lock=True)


def destroy_video_shared_state():
    """
    Release the parent's reference to the shared video state object.
    mp.Value does not need an explicit free, but clearing the global reference
    makes the lifetime of the object easier to reason about.
    """
    global video_state
    video_state = None


def set_video_state(state_obj, new_state: int):
    """
    Safely update the shared video-process state.
    """
    if state_obj is None:
        return

    with state_obj.get_lock():
        state_obj.value = new_state


def get_video_state(state_obj) -> int:
    """
    Safely read the shared video-process state.
    """
    if state_obj is None:
        return VIDEO_STATE_STOPPED

    with state_obj.get_lock():
        return state_obj.value


def video_state_name(state: int) -> str:
    """
    Convert a numeric video state into a readable debug string.
    """
    if state == VIDEO_STATE_STOPPED:
        return "STOPPED"
    if state == VIDEO_STATE_STARTING:
        return "STARTING"
    if state == VIDEO_STATE_WAITING:
        return "WAITING"
    if state == VIDEO_STATE_STREAMING:
        return "STREAMING"
    if state == VIDEO_STATE_STOPPING:
        return "STOPPING"
    if state == VIDEO_STATE_ERROR:
        return "ERROR"
    return f"UNKNOWN({state})"


def video_is_active() -> bool:
    """
    Returns True if the video process is in some active/running state.
    """
    state = get_video_state(video_state)
    return state in (
        VIDEO_STATE_STARTING,
        VIDEO_STATE_WAITING,
        VIDEO_STATE_STREAMING,
        VIDEO_STATE_STOPPING,
    )


def refresh_video_process_status() -> int:
    """
    Synchronize the shared state with the actual child process object.
    This lets the control process notice if the video process died unexpectedly.
    """
    global stream_proc
    global video_state

    if stream_proc is None:
        if video_state is not None:
            set_video_state(video_state, VIDEO_STATE_STOPPED)
        return VIDEO_STATE_STOPPED

    #if the process object exists but is no longer alive, update shared state
    if not stream_proc.is_alive():
        current = get_video_state(video_state)

        #preserve explicit error state if the child marked one
        if current != VIDEO_STATE_ERROR:
            set_video_state(video_state, VIDEO_STATE_STOPPED)

        return get_video_state(video_state)

    return get_video_state(video_state)


def cleanup_video_process():
    """
    Stop the video process if it exists and reset the shared state.
    Safe to call multiple times.
    """
    global stream_proc
    global video_state

    if video_state is not None:
        current = get_video_state(video_state)
        if current not in (VIDEO_STATE_STOPPED, VIDEO_STATE_ERROR):
            set_video_state(video_state, VIDEO_STATE_STOPPING)

    if stream_proc is not None:
        try:
            if stream_proc.is_alive():
                stream_proc.terminate()
                stream_proc.join(timeout=1.0)
        except Exception:
            pass

        try:
            stream_proc.close()
        except Exception:
            pass

        stream_proc = None

    if video_state is not None:
        set_video_state(video_state, VIDEO_STATE_STOPPED)

    destroy_video_shared_state()


def start_video_process():
    """
    Create shared state, launch the video process, and mark it as starting.
    """
    global stream_proc
    global video_state

    #clean up any stale state from a previous run
    cleanup_video_process()

    video_state = create_video_shared_state()
    set_video_state(video_state, VIDEO_STATE_STARTING)

    stream_proc = mp.Process(
        target=video_server_main,
        args=(video_state,),
        daemon=True,
        name="VideoProc"
    )
    stream_proc.start()

def shutdown():
    """
    Handles graceful shutdown of daemon process and servers
    """
    global ser

    print("[CONTROL] Shutting down...")

    #shutdown serial connection
    try:
        if ser is not None and ser.is_open:
            ser.close()
    except Exception:
        pass

    if STREAM_VIDEO and stream_proc is not None:
        cleanup_video_process()
    
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
    """
    Launch ffmpeg and stream H.264 Annex-B bytes to stdout.
    """

    vf = (
        "scale=400:240:force_original_aspect_ratio=decrease,"
        "pad=400:240:(400-iw)/2:(240-ih)/2"
    )

    cmd = [
        "ffmpeg",
        "-loglevel", "error",

        # Webcam input on Windows
        "-f", "dshow",
        "-video_size", "640x480",
        "-framerate", "30",
        "-i", f"video={device_name}",

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
def video_server_main(shared_video_state):
    """
    Separate process for serving the H.264 video stream to the 3DS.
    Uses shared memory so the parent process can track its state.
    """

    conn = None
    srv = None
    proc = None

    try:
        #create TCP server socket
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", VIDEO_PORT))
        srv.settimeout(0.5)
        srv.listen(1)

        set_video_state(shared_video_state, VIDEO_STATE_WAITING)
        print(f"[VIDEO] Listening on 0.0.0.0:{VIDEO_PORT}")
        print(f"[VIDEO] Waiting for client...")

        #loop to keep from blocking
        while True:
            try:
                conn, addr = srv.accept()
                break
            except socket.timeout:
                continue
            
        print(f"[VIDEO] Client connected from {addr}")

        #conduct handshake, like a true gentleman
        hello = conn.recv(6) #6 bytes, expects 'YOINK'
        if hello != b"YOINK\n":
            print(f"[VIDEO] Bad handshake: got {hello!r}, expected b'YOINK'")
            set_video_state(shared_video_state, VIDEO_STATE_ERROR)
            return
        
        conn.sendall(b"YEET\n")
        print("[VIDEO] Handshake OK (YOINK <-> YEET)")

        #start ffmpeg
        proc = ffmpeg_h264_stream_webcam(WEBCAM)
        assert proc.stdout is not None

        set_video_state(shared_video_state, VIDEO_STATE_STREAMING)
    
        rolling = b""                   #rolling buffer for ffmpeg bytes before they are split into NAL units
        nal_queue: List[bytes] = []     #queue of complete NAL units ready to send

        #stream loop
        while True:
            #ensure at least one complete NAL is available
            while not nal_queue:
                chunk = proc.stdout.read(8192)
                if not chunk:
                    #ffmpeg ended; send end-of-stream marker
                    try:
                        conn.sendall(struct.pack(">I", 0))
                    except Exception:
                        pass
                    print("[VIDEO] End-of-stream sent (length=0).")
                    set_video_state(shared_video_state, VIDEO_STATE_STOPPING)
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
                print(f"[VIDEO] client connection closed: {e}")
                break

    except KeyboardInterrupt:
        print("[VIDEO] Keyboard interrupt!")    
        set_video_state(shared_video_state, VIDEO_STATE_STOPPING)

    except Exception as e:
        print(f"[VIDEO] Unhandled exception: {e}")
        set_video_state(shared_video_state, VIDEO_STATE_ERROR)

    finally:
        #Cleanup
        print("[VIDEO] Initiating cleanup...")
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass

        if srv is not None:
            try:
                srv.close()
            except Exception:
                pass

        if proc is not None:
            try:
                proc.kill()
            except Exception:
                pass

            try:
                proc.wait(timeout=1.0)
            except Exception:
                pass
        
        #if the daemon did not explicitly report an error, mark it stopped
        if get_video_state(shared_video_state) != VIDEO_STATE_ERROR:
            set_video_state(shared_video_state, VIDEO_STATE_STOPPED)
        print("[VIDEO] Clean shutdown.")
#==================================================

def main():
    global stream_proc
    global ser

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
        s.settimeout(0.5)
        print(f"[CONTROL] Listening on {HOST}:{PORT} ...")

        #loop to prevent listening from keeping app open
        while True:
            try:
                conn, addr = s.accept()
                break
            except socket.timeout:
                continue

        with conn:
            print(f"[CONTROL] Client connected from {addr}")

            #control handshake, as is right and just
            while True:
                try:
                    hello = conn.recv(1024)
                    break
                except socket.timeout:
                    continue

            if hello != b"GREETINGS\n":
                print(f"[CONTROL] Bad handshake: got {hello!r}, expected b'GREETINGS'")
                conn.close()
                return

            conn.sendall(b"WHORE")

            running = True
            last_command_sent = None
            last_video_state = VIDEO_STATE_STOPPED

            if STREAM_VIDEO:
                #now start video streaming process
                start_video_process()
                last_video_state = get_video_state(video_state)
                print(f"[CONTROL] Video process state: {video_state_name(last_video_state)}")

            try:
                while running:
                    #keep parent-side knowledge of the video process up to date
                    if STREAM_VIDEO:
                        state_now = refresh_video_process_status()
                        if state_now != last_video_state:
                            print(f"[CONTROL] Video state changed: {video_state_name(last_video_state)} -> {video_state_name(state_now)}")
                            last_video_state = state_now

                    #receive command
                    command = conn.recv(2)

                    #3ds closed the connection
                    if not command:
                        print("[CONTROL] 3DS disconnected.")
                        break

                    if command != last_command_sent:
                        ser.write(command)
                        last_command_sent = command
                        #print(f"[CONTROL] Sent command: {command}")
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

                    #limit how fast commands are sent
                    time.sleep(1.0 / SEND_RATE_HZ)
            except KeyboardInterrupt:
                print("[CONTROL] Keyboard interrupt.\n")

            finally:
                try:
                    shutdown()
                except Exception:
                    pass

                print("\n[CONTROL] Disconnected.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        shutdown()
        print("\nShutting down...")