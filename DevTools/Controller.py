"""
Rokenbok keyboard controller for Arduino over serial

Sends commands to the arduino, but currently only supports 1 command at a time
"""

import sys
import time
import serial
import pygame


# ============================================================
# User settings
# ============================================================

# Change this to your Arduino's serial port
SERIAL_PORT = "COM5"

# Must match Serial.begin(...) in the Arduino sketch
BAUD_RATE = 250000

# How often to send command updates (times per second)
SEND_RATE_HZ = 30

#whether to operate in single command mode or multipile command mode
MULTI_COMMAND = True


# ============================================================
# Key-to-command mapping
# These characters match the Arduino code you posted earlier.
# ============================================================

KEY_TO_COMMAND = {
    pygame.K_w: 'w',   # forward
    pygame.K_s: 's',   # backward
    pygame.K_a: 'a',   # left
    pygame.K_d: 'd',   # right

    pygame.K_i: 'i',   # lift up
    pygame.K_k: 'k',   # lift down
    pygame.K_j: 'j',   # grab close
    pygame.K_l: 'l',   # grab open

    pygame.K_SPACE: '0',  # select
}

IDLE_COMMAND = '!'   # tells Arduino to go idle / stop
QUIT_KEYS = {pygame.K_ESCAPE}


# ============================================================
# Helper: choose which command should be sent right now
# ============================================================

def get_active_command(keys_pressed: pygame.key.ScancodeWrapper) -> str:
    """
    Decide which command to send based on the current keyboard state.

    Priority order matters here.
    For example, if W and A are both held, this function currently
    gives priority to W because it appears first.

    You can change the order if you want turning to override movement.
    """

    priority_order = [
        pygame.K_w,
        pygame.K_s,
        pygame.K_a,
        pygame.K_d,
        pygame.K_i,
        pygame.K_k,
        pygame.K_j,
        pygame.K_l,
        pygame.K_SPACE,
    ]

    for key in priority_order:
        if keys_pressed[key]:
            return KEY_TO_COMMAND[key]

    return IDLE_COMMAND

# ============================================================
# Identify which buttons are held down now
# ============================================================

def get_held_buttons(keys_pressed: pygame.key.ScancodeWrapper) -> list[bool]:
    """
    returns a 16 character string that holds the status of the keys
    placement is based on pulse timing
    """

    working_list = [False, False, False, False, False, False, False, False,
                    False, False, False, False, False, False, False, False]

    #select button
    if keys_pressed[pygame.K_SPACE]:
        working_list[14] = True

    #dpad up
    if keys_pressed[pygame.K_w]:
        working_list[9] = True

    #dpad down
    if keys_pressed[pygame.K_s]:
        working_list[8] = True
    
    #dpad right
    if keys_pressed[pygame.K_d]:
        working_list[7] = True

    #dpad left
    if keys_pressed[pygame.K_a]:
        working_list[6] = True

    if keys_pressed[pygame.K_i]:
        working_list[5] = True

    if keys_pressed[pygame.K_k]:
        working_list[4] = True

    if keys_pressed[pygame.K_j]:
        working_list[0] = True

    if keys_pressed[pygame.K_l]:
        working_list[1] = True

    output = bool_aray_to_bytes(working_list)

    return output

# ============================================================
# Convert array of bools into raw bytes
# ============================================================

def bool_aray_to_bytes(arr):
    #output needs to be a multiple of 8, so add padding as necessary
    padding = (8 - len(arr) % 8) % 8
    padded_arr = arr + [False] * padding

    #create byte array
    byte_arr = bytearray()
    for i in range(0, len(padded_arr), 8): #from 0 to last index, increment by 8
        byte = 0
        for j in range(8):
            if padded_arr[i+j]:
                byte |= (1 << (7 - j))
        byte_arr.append(byte)
    return byte_arr



# ============================================================
# Main program
# ============================================================

def main():
    # --------------------------------------------------------
    # Open serial connection to Arduino
    # --------------------------------------------------------
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0)
    except serial.SerialException as e:
        print(f"Failed to open serial port {SERIAL_PORT}: {e}")
        sys.exit(1)

    # Give the Arduino a moment.
    # Many Arduino boards reset when the serial port opens.
    time.sleep(2.0)

    print(f"Connected to {SERIAL_PORT} at {BAUD_RATE} baud.")
    print("Controls:")
    print("  W/A/S/D = move")
    print("  I/K     = lift up/down")
    print("  J/L     = grab close/open")
    print("  SPACE   = select")
    print("  ESC     = quit")

    # --------------------------------------------------------
    # Initialize pygame
    # --------------------------------------------------------
    pygame.init()

    # We need a window for pygame keyboard handling to work reliably.
    screen = pygame.display.set_mode((640, 480))
    pygame.display.set_caption("Rokenbok Controller")

    font = pygame.font.SysFont(None, 28)
    clock = pygame.time.Clock()

    running = True
    last_command_sent = None

    try:
        while running:
            # ------------------------------------------------
            # Process pygame events
            # ------------------------------------------------
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    running = False

                elif event.type == pygame.KEYDOWN:
                    if event.key in QUIT_KEYS:
                        running = False

            # ------------------------------------------------
            # Read current keyboard state
            # ------------------------------------------------
            keys = pygame.key.get_pressed()
            if MULTI_COMMAND:
                command = get_held_buttons(keys)

                if command != last_command_sent:
                    ser.write(command)
                    last_command_sent = command
                    print(f"Sent command: {command}")
                else:
                    ser.write(command)
            else:
                command = get_active_command(keys)

                # ------------------------------------------------
                # Send command if changed, or periodically resend it
                # ------------------------------------------------
                # Re-sending helps keep the Arduino updated while a key is held.
                if command != last_command_sent:
                    ser.write(command.encode('ascii'))
                    last_command_sent = command
                    print(f"Sent command: {repr(command)}")
                else:
                    ser.write(command.encode('ascii'))

            # ------------------------------------------------
            # Optional: read back any Arduino serial output
            # ------------------------------------------------
            # If your Arduino prints status messages, this displays them.
            try:
                waiting = ser.in_waiting
                if waiting > 0:
                    incoming = ser.read(waiting).decode(errors="ignore")
                    if incoming.strip():
                        print(incoming, end="")
            except Exception:
                # Ignore serial decode/read hiccups
                pass

            # ------------------------------------------------
            # Draw a tiny status UI
            # ------------------------------------------------
            screen.fill((30, 30, 30))

            lines = [
                "Rokenbok Keyboard Controller",
                "",
                "W/A/S/D = move",
                "I/K = lift up/down",
                "J/L = grab close/open",
                "SPACE = select",
                "ESC = quit",
                "",
                f"Current command: {repr(command)}",
                f"Port: {SERIAL_PORT}",
            ]

            y = 15
            for line in lines:
                text_surface = font.render(line, True, (220, 220, 220))
                screen.blit(text_surface, (20, y))
                y += 22

            pygame.display.flip()

            # Limit loop speed
            clock.tick(SEND_RATE_HZ)

    finally:
        # ----------------------------------------------------
        # On exit, send idle command so the vehicle stops
        # ----------------------------------------------------
        try:
            ser.write(IDLE_COMMAND.encode('ascii'))
            time.sleep(0.05)
        except Exception:
            pass

        try:
            ser.close()
        except Exception:
            pass

        pygame.quit()
        print("\nDisconnected. Sent stop/idle command.")


if __name__ == "__main__":
    main()