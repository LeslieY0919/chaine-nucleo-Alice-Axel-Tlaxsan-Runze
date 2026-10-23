#!/usr/bin/env python3
"""PC sortie : reçoit les trames @DN de la dernière carte et renvoie les ACK."""
import re
import select
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "transmitter"))
from console import Serial, frame, decode

INIT, RESET, ACK, MSG, ERROR = range(5)


class PcSortie:
    def __init__(self, serial, out=print):
        self.serial = serial
        self.out = out
        self.session = None
        self.cipher = None
        self.messages = []

    def reply(self, command, phase):
        if self.session is None:
            self.out("Attention : session inconnue, relancer /init sur le maître")
            return
        payload = bytes([command, phase, 0]) + self.session
        self.serial.send("/dn " + frame(ACK, payload, source=0).hex())

    def handle(self, line):
        if line.startswith("ETAT="):
            match = re.search(r"SESSION=([0-9a-f]{8})", line)
            if match and int(match.group(1), 16):
                self.session = bytes.fromhex(match.group(1))
            return
        match = re.match(r"DECODE : recu ([0-9a-f]+) ->", line)
        if match:
            self.cipher = match.group(1)
            return
        if not line.startswith("@DN "):
            return
        try:
            command, payload, source = decode(bytes.fromhex(line[4:]))
        except ValueError as error:
            self.out(f"Attention : trame rejetée ({error})")
            return

        if command == INIT and payload[0] in (2, 3):
            self.session = payload[12:16]
            self.reply(INIT, payload[0])
            if payload[0] == 2:
                self.out(f"Bien reçu ! Vérification : la carte {source} cherche le PC sortie")
            else:
                self.out(f"Bien reçu ! Synchronisation : session {self.session.hex()}")
                self.out("    Chaîne prête, en attente des messages\n")
        elif command == RESET:
            self.reply(RESET, 0)
            self.out("Bien reçu ! Réinitialisation : demandée par le maître\n")
        elif command == MSG:
            self.messages.append(payload)
            self.out("-" * 40)
            self.out(f"Bien reçu ! Message : {payload.decode('utf-8', errors='replace')}")
            if self.cipher:
                self.out(f"    Chiffré  : {self.cipher}")
            self.out(f"    Longueur : {len(payload)} octets")
            self.out("    ACK envoyé au maître\n")
            self.cipher = None
            self.reply(MSG, 0)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage : python3 slave/sortie.py /dev/cu.usbmodemXXXX")
    serial = Serial(sys.argv[1])
    pc = PcSortie(serial)
    print(f"PC sortie : {sys.argv[1]}  (Ctrl+C pour quitter)\n")
    serial.send("/status")
    try:
        while True:
            ready = select.select([serial.fd, sys.stdin], [], [], 0.05)[0]
            while True:
                line = serial.read_line(0.01)
                if line is None:
                    break
                if line.startswith("ETAT="):
                    print(f"État de la carte : {line.split()[0][5:]}\n")
                elif line.startswith("USER : "):
                    print("Bouton " + line)
                elif "ERREUR" in line:
                    print("Attention : " + line)
                pc.handle(line)
            if sys.stdin in ready:
                typed = sys.stdin.readline()
                if typed:
                    serial.send(typed.strip())
    except KeyboardInterrupt:
        print(f"\nFin : {len(pc.messages)} message(s) reçu(s)")
    finally:
        serial.close()


if __name__ == "__main__":
    main()
