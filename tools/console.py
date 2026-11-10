"""Send commands to the display's USB console and print the replies.

usage: python tools/console.py "status" "maint {\"odometer\":152340}" ...
A command of the form "wait 500" pauses for that many milliseconds.
"""
import sys
import time
import serial

s = serial.Serial()
s.port, s.baudrate, s.timeout = "COM3", 115200, 0.3
s.dtr = s.rts = False  # don't reset the board
s.open()
s.reset_input_buffer()
for cmd in sys.argv[1:]:
    if cmd.startswith("wait "):
        time.sleep(int(cmd.split()[1]) / 1000)
        continue
    s.write((cmd + "\n").encode())
    time.sleep(0.4)
    print(s.read(20000).decode(errors="replace"), end="")
s.close()
