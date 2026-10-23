#!/usr/bin/env python3
"""Console du maître ; --simulate remplace la chaîne par un récepteur sur PC."""
import argparse
import binascii
import fcntl
import os
import select
import sys
import termios
import time
import tty


class Serial:
    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.previous = None
        try:
            fcntl.ioctl(self.fd, termios.TIOCEXCL)
            self.previous = termios.tcgetattr(self.fd)
            tty.setraw(self.fd)
            settings = termios.tcgetattr(self.fd)
            settings[2] |= termios.CLOCAL | termios.CREAD
            settings[2] &= ~getattr(termios, "CRTSCTS", 0)
            settings[4] = settings[5] = termios.B115200
            termios.tcsetattr(self.fd, termios.TCSANOW, settings)
            termios.tcflush(self.fd, termios.TCIFLUSH)
            self.buffer = bytearray()
            for attempt in range(3):
                self.send("\n/status")
                deadline = time.monotonic() + 0.5
                connected = False
                while time.monotonic() < deadline:
                    line = self.read_line(0.1)
                    if line is not None and line.startswith("ETAT="):
                        connected = True
                        break
                if connected:
                    break
            else:
                raise ConnectionError("Pas de réponse du firmware maître")
        except BaseException:
            self.close()
            raise

    def close(self):
        try:
            termios.tcdrain(self.fd)
            if self.previous is not None:
                termios.tcsetattr(self.fd, termios.TCSANOW, self.previous)
            fcntl.ioctl(self.fd, termios.TIOCNXCL)
        finally:
            os.close(self.fd)

    def send(self, line):
        remaining = (line + "\n").encode("utf-8")
        deadline = time.monotonic() + 2
        while remaining:
            if time.monotonic() > deadline:
                raise TimeoutError("Écriture série bloquée")
            if select.select([], [self.fd], [], 0.1)[1]:
                remaining = remaining[os.write(self.fd, remaining):]

    def read_line(self, timeout=0.1):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                return None
            block = os.read(self.fd, 4096)
            if not block:
                raise ConnectionError("Carte déconnectée")
            self.buffer.extend(block)
        line, _, rest = self.buffer.partition(b"\n")
        self.buffer = bytearray(rest)
        return line.rstrip(b"\r").decode("utf-8", errors="replace")


def frame(command, payload=b"", source=1):
    if len(payload) > 16:
        raise ValueError("Payload trop long")
    body = bytes([0x7E, command, len(payload), 0]) + source.to_bytes(4, "big")
    body += payload.ljust(16, b"\0")
    return body + binascii.crc_hqx(body, 0xFFFF).to_bytes(2, "big")


def decode(raw):
    if len(raw) != 26 or raw[0] != 0x7E or raw[2] > 16 or raw[3] != 0:
        raise ValueError("Format invalide")
    if binascii.crc_hqx(raw[:24], 0xFFFF) != int.from_bytes(raw[24:], "big"):
        raise ValueError("CRC invalide")
    if any(raw[8 + raw[2]:24]):
        raise ValueError("Padding non nul")
    return raw[1], raw[8:8 + raw[2]], int.from_bytes(raw[4:8], "big")


class Peer:
    def __init__(self):
        self.session = bytes(4)
        self.config = {}
        self.nodes = 0
        self.ready = False
        self.messages = []

    def reply(self, raw):
        command, p, source = decode(raw)
        if source != 0:
            raise ValueError("Le maître doit utiliser ID 0")
        if command == 0:
            if len(p) != 16 or p[3] or p[0] not in (1, 2, 3):
                raise ValueError("INIT invalide")
            phase, target, nodes = p[:3]
            if not 1 <= nodes <= 16:
                raise ValueError("Nombre de cartes invalide")
            if phase == 1:
                if target == 1:
                    self.config = {}
                    self.session, self.nodes, self.ready = p[12:16], nodes, False
                if p[12:16] != self.session or nodes != self.nodes or target != len(self.config) + 1:
                    raise ValueError("Ordre de configuration invalide")
                inbound, outbound = p[4:8], p[8:12]
                if inbound == bytes(4) or (target == nodes) != (outbound == bytes(4)):
                    raise ValueError("Clé invalide")
                if target > 1 and inbound != self.config[target - 1][1]:
                    raise ValueError("Clés voisines incompatibles")
                self.config[target] = (inbound, outbound)
            else:
                if (target or any(p[4:12]) or p[12:16] != self.session
                        or nodes != self.nodes or len(self.config) != nodes):
                    raise ValueError("Chaîne incomplète")
                self.ready = phase == 3
            return frame(2, bytes([0, phase, target]) + self.session)
        if command == 1:
            response = frame(2, bytes([1, 0, 0]) + self.session)
            self.ready = False
            self.config = {}
            return response
        if command == 3:
            if not self.ready:
                raise ValueError("MSG avant la synchronisation")
            key = self.config[1][0]
            clear = bytes(b ^ key[i % 4] for i, b in enumerate(p))
            self.messages.append(clear)
            return frame(2, bytes([3, 0, 0]) + self.session)
        if command in (2, 4):
            return None
        raise ValueError("Commande inconnue")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", default="/dev/cu.usbmodem11303")
    parser.add_argument("--simulate", action="store_true")
    parser.add_argument("--nodes", type=int, default=1, choices=range(1, 17))
    args = parser.parse_args()
    serial = Serial(args.port)
    peer = Peer() if args.simulate else None
    try:
        serial.send("/test on" if peer else "/status")
        if peer:
            print("SIMULATION : la carte est réelle ; les esclaves et le PC sortie sont simulés.")
            serial.send(f"/init {args.nodes}")
        print("Saisir un texte (16 octets maximum), /status, /reset ou /help. Ctrl+C pour quitter.")
        while True:
            line = serial.read_line(0.05)
            if line is not None:
                print(line, flush=True)
                if peer and line.startswith("@TX "):
                    before = len(peer.messages)
                    response = peer.reply(bytes.fromhex(line[4:]))
                    if len(peer.messages) != before:
                        print("PC sortie simulé :", peer.messages[-1].decode("utf-8", errors="replace"), flush=True)
                    if response:
                        serial.send("/rx " + response.hex())
            if select.select([sys.stdin], [], [], 0)[0]:
                line = sys.stdin.readline()
                if not line:
                    break
                serial.send(line.rstrip("\r\n"))
    except KeyboardInterrupt:
        print("\nConsole fermée.")
    finally:
        if peer:
            serial.send("/test off")
        serial.close()


if __name__ == "__main__":
    main()
