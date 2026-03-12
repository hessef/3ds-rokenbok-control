"""
Server to direct commands from the 3DS to the arduino for the Rockenbok
"""

import sys
import time
import serial
import socket
import struct
from pathlib import Path


#=========================CONFIGURATION SETTINGS=========================

#serial connection
SERIAL_PORT = "COM5"
BAUD_RATE = 250000
SEND_RATE_HZ = 30

#network connection
HOST = "0.0.0.0"
PORT = 5000
#==================================================

def main():
    # Open serial connection to Arduino
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0)
    except serial.SerialException as e:
        print(f"Failed to open serial port {SERIAL_PORT}: {e}")
        sys.exit(1)

    # Give the Arduino a moment.
    # Many Arduino boards reset when the serial port opens.
    time.sleep(2.0)

    print(f"Connected to {SERIAL_PORT} at {BAUD_RATE} baud.")

    #open websocket and connect to 3ds
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(1)
        print(f"Listening on {HOST}:{PORT} ...")

        conn, addr = s.accept()
        with conn:
            print(f"Client connected from {addr}")

            data = conn.recv(1024)
            print(f"Handshake recv: {data!r}")

            conn.sendall(b"WHORE\n")

            running = True
            last_command_sent = None

            try:
                while running:
                    #receive command
                    command = conn.recv(2)

                    #3ds closed the connection
                    if not command:
                        print("3DS disconnected.")
                        break

                    if command != last_command_sent:
                        ser.write(command)
                        last_command_sent = command
                        print(f"Sent command: {command}")
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

                print("\nDisconnected.")


if __name__ == "__main__":
    main()