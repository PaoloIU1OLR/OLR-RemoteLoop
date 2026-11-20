# OLR-RemoteLoop

Comando wireless per una **loop magnetica** (il condensatore variabile ad aria) via motore passo-passo, pilotabile **dal solo cellulare** tramite pagina web — nessun dispositivo aggiuntivo richiesto (niente Cardputer, niente PC sul campo). Pensato per attività radioamatoriali in portatile (POTA, SOTA, QRP all'aperto).

Autore: **IU1OLR**

## Cos'è

Una ESP32 pilota un motore passo-passo 28BYJ-48 (tramite driver ULN2003) che ruota il condensatore variabile della loop magnetica. L'ESP32 fa da Access Point WiFi e serve direttamente dal suo firmware una pagina web di controllo completa: posizione a mano, memorie di banda/frequenza preferite, limiti meccanici, zero di riferimento. Non serve installare nulla sul telefono: basta collegarsi al WiFi dell'ESP32 e aprire una pagina nel browser.

La stessa ESP32 può anche essere comandata via ESP-NOW da un M5Stack Cardputer (vedi il progetto collegato più sotto) — ma funziona perfettamente anche da sola, con il solo telefono.

## Di cosa hai bisogno (BOM)

- 1× ESP32 (va bene un generico "ESP32 Dev Module", es. WROOM 38 pin, con USB-C per comodità di alimentazione da power bank)
- 1× motore passo-passo **28BYJ-48** (5V) + driver **ULN2003**
- Un accoppiamento meccanico tra il motore e l'asse del condensatore variabile della tua loop (dipende dal modello di loop — non è oggetto di questo repository)
- Power bank USB-C (alimenta l'ESP32, che a sua volta alimenta il driver/motore dai propri pin V5/GND)

Non sono richiesti finecorsa fisici: zero e limiti di rotazione si impostano via software dalla pagina web.

## Firmware (cartella `firmware/`)

- `OLR_LoopRX_v2.0.ino` — sketch principale
- `OLR_LoopProto.h` — definizioni condivise (struttura dei comandi, 8 memorie disponibili)

**Come compilare:**

1. Arduino IDE, con il supporto schede **ESP32** installato (Boards Manager → "esp32" di Espressif)
2. Scheda: **ESP32 Dev Module**
3. Apri `OLR_LoopRX_v2.0.ino`, compila e carica via USB — non servono librerie esterne oltre a quelle incluse nel core ESP32

**Collegamento motore** (pin di default IN1..IN4, modificabili in testa allo sketch):

| Driver ULN2003 | Pin ESP32 |
|---|---|
| IN1 | GPIO 14 |
| IN2 | GPIO 27 |
| IN3 | GPIO 26 |
| IN4 | GPIO 25 |
| + (alimentazione) | V5 dell'ESP32 |
| − | GND dell'ESP32 |

## Primo avvio e uso

1. Accendi l'ESP32: crea una rete WiFi propria, SSID **`OLR_Loop`**, password **`loopmagn`** (modificabili dalla pagina web stessa, voce WiFi)
2. Collega il telefono a quella rete
3. Apri nel browser **`http://192.168.4.1/`**
4. **Imposta lo zero**: porta manualmente il condensatore nella posizione che vuoi usare come riferimento e premi "Zero qui"
5. **Imposta i limiti meccanici** di rotazione (minimo e massimo), per evitare che il motore forzi il condensatore a fine corsa
6. Usa le **8 memorie** per salvare le posizioni delle bande/frequenze che usi più spesso, per richiamarle con un tocco
7. Muovi/posiziona il condensatore a mano (frecce o inserimento diretto della posizione) durante la sintonia

> Nota: la loop magnetica di riferimento usata in sviluppo richiede circa 2-3 giri completi (720°-1080°) del motore 28BYJ-48 per coprire tutta la corsa del condensatore — tienilo a mente se la tua meccanica ha un rapporto diverso, i limiti via software si adattano a qualunque escursione.

## Licenza

CC BY-NC-SA 4.0 — vedi [`LICENSE`](LICENSE). Uso e modifica liberi, non commerciale, stessa licenza per i derivati. Se modifichi o migliori il progetto, una pull request o un'issue su questo repository sono il modo più apprezzato per farlo sapere all'autore.

## Progetto collegato

Questo comando per loop magnetica è stato sviluppato come estensione di **[SuiteQRPCardputer](https://github.com/PaoloIU1OLR/SuiteQRPCardputer)**, il firmware per M5Stack Cardputer ADV dell'autore (log QSO, CW Trainer, tracking satelliti con Icom IC-705). Funziona anche da solo col Cardputer (via ESP-NOW) — questo repository documenta la via standalone via cellulare per chi non possiede un Cardputer. Vedi anche **[OLR-SatRotor](https://github.com/PaoloIU1OLR/OLR-SatRotor)**, lo stesso approccio applicato a un rotore az/el satellitare.
