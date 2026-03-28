"""
send_command.py -- Send control commands to the ESP32 over TCP (port 8082).

Commands:
    PLAY              Resume / start playback
    PAUSE             Pause playback (TCP sender blocks naturally via flow control)
    STOP              Stop playback and clear the audio buffer
    VOLUME <0-100>    Set playback volume (100 = full, 0 = muted)

Usage:
    python send_command.py <host> <command> [port]

Examples:
    python send_command.py 192.168.1.100 PAUSE
    python send_command.py 192.168.1.100 PLAY
    python send_command.py 192.168.1.100 STOP
    python send_command.py 192.168.1.100 "VOLUME 50"
    python send_command.py 192.168.1.100 "VOLUME 100"
"""

import sys
import socket

CMD_PORT = 8082


def send_command(host: str, port: int, command: str) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(5)
        s.connect((host, port))
        s.sendall((command.strip() + "\n").encode())
    print(f"Sent to {host}:{port} -> {command.strip()}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    host    = sys.argv[1]
    command = sys.argv[2]
    port    = int(sys.argv[3]) if len(sys.argv) > 3 else CMD_PORT

    send_command(host, port, command)
