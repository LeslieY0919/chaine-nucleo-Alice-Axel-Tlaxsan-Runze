# Chaîne d'information résiliente — NUCLEO-F446RE

PC entrée → **maître** → **esclave(s)** → PC sortie, de proche en proche par UART (115 200 bauds).

- **Maître** (`transmitter/`) : reçoit le texte du PC, génère une clé de 32 bits par lien, configure la chaîne et chiffre les messages par XOR.
- **Esclaves** (`slave/`) : un seul firmware ; la position, l'ID et les clés sont reçus du maître. Un relais vérifie le CRC, déchiffre puis rechiffre avec la clé du lien suivant ; la dernière carte déchiffre et envoie le texte en clair au PC sortie (`slave/sortie.py`), qui renvoie un ACK jusqu'au maître.
- **Initialisation** (`/init N`) : CONFIG de chaque carte, PROBE jusqu'au PC sortie, COMMIT ; la dernière carte est désignée explicitement, jamais déduite d'un silence.
- **Sécurité de la chaîne** : trames de 26 octets (`protocol_icd.h`) avec CRC16 ; une trame corrompue, un voisin inattendu (permutation de cartes) ou une carte disparue provoquent une erreur ou un timeout, jusqu'à une réinitialisation (`/reset` ou 3 appuis sur USER).
- **LED** : 0,5 Hz en attente, 2 Hz en synchronisation, fixe quand la chaîne est prête, 5 Hz en cas d'erreur. Un appui sur USER affiche le rôle de la carte.
- **Câblage** (CN7) : carte k CN7-1 (TX) → carte k+1 CN7-4 (RX), carte k CN7-2 (RX) ← carte k+1 CN7-3 (TX), masse commune.

**Compilation** : projet STM32CubeIDE pour NUCLEO-F446RE (HAL STM32CubeF4), avec `protocol.c` et les fichiers `.c` de `transmitter/` (maître) ou de `slave/` (esclave) ; flash avec STM32CubeProgrammer.

**Démonstration** : `python3 slave/sortie.py PORT_ESCLAVE`, puis `python3 transmitter/console.py PORT_MAITRE`, `/init N` et du texte.

Validé sur cartes (maître + esclave) le 23/10/2026 : journal dans `slave/evidence/chaine_hardware.json`.
