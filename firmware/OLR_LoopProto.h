// OLR_LoopProto.h - protocollo ESP-NOW tra Cardputer (OLR Suite) e ESP32 della Loop
// Versione protocollo 2. Lo stesso file va incluso anche nel firmware del Cardputer.
//
// Regole:
//  - Il Cardputer invia sempre un LoopCmd (24 byte).
//  - L'ESP32 della loop risponde con LoopStatus (sempre) e con LoopMemList
//    dopo PAIR, GET, MEM_SET e MEM_LIST.
//  - Tutti i dati (posizione, limiti, memorie, gioco) stanno SOLO sull'ESP32 della loop.
//  - seq: il Cardputer lo incrementa a ogni nuovo comando. Se ritrasmette lo stesso
//    comando (nessuna risposta) usa lo stesso seq: la loop non lo riesegue ma risponde.
//  - Canale: il Cardputer deve trasmettere sul canale WiFi della loop (default 1).
#pragma once
#include <stdint.h>

#define LOOP_MAGIC      0xBC
#define LOOP_PROTO      2
#define LOOP_MEM_N      8
#define LOOP_NAME_LEN   12   // compreso lo zero finale

// Comandi (LoopCmd.type)
enum : uint8_t {
    LCMD_MOVE      = 1,   // value = passi relativi (+/-)
    LCMD_GOTO      = 2,   // value = posizione assoluta
    LCMD_SET_ZERO  = 3,   // la posizione attuale diventa 0 (calibrazione)
    LCMD_STOP      = 4,
    LCMD_GET       = 5,   // chiede stato + memorie
    LCMD_SET_MIN   = 6,   // value = limite minimo
    LCMD_SET_MAX   = 7,   // value = limite massimo
    LCMD_SET_POS   = 8,   // value = dichiara la posizione attuale (calibrazione, non muove)
    LCMD_PAIR      = 9,   // abbinamento (inviare in broadcast FF:FF:FF:FF:FF:FF)
    LCMD_MEM_GO    = 10,  // idx = memoria 0..7
    LCMD_MEM_SET   = 11,  // idx, name, value (value = LOOP_POS_CURRENT -> posizione attuale)
    LCMD_MEM_LIST  = 12,  // chiede solo le memorie
    LCMD_SET_BACKLASH = 13 // value = passi di compensazione gioco (0 = disattivata)
};

// Risposte (type)
enum : uint8_t {
    LRSP_STATUS   = 0xA0,
    LRSP_MEMLIST  = 0xA1
};

// Flag in LoopStatus.flags
#define LFLAG_MOVING     0x01
#define LFLAG_UNCERTAIN  0x02  // spenta durante un movimento: posizione da ricalibrare
#define LFLAG_PAIRED     0x04

#define LOOP_POS_CURRENT ((int32_t)0x7FFFFFFF)

struct __attribute__((packed)) LoopCmd {
    uint8_t  magic;      // LOOP_MAGIC
    uint8_t  type;       // LCMD_*
    uint8_t  proto;      // LOOP_PROTO
    uint8_t  idx;        // indice memoria (MEM_GO / MEM_SET)
    int32_t  value;
    uint32_t seq;
    char     name[LOOP_NAME_LEN]; // solo MEM_SET
};  // 24 byte

struct __attribute__((packed)) LoopStatus {
    uint8_t  magic;      // LOOP_MAGIC
    uint8_t  type;       // LRSP_STATUS
    uint8_t  proto;
    uint8_t  flags;      // LFLAG_*
    int32_t  position;
    int32_t  target;     // destinazione finale
    int32_t  minLimit;
    int32_t  maxLimit;
    int16_t  backlash;
    uint16_t fwVersion;  // es. 200 = v2.00
    uint32_t lastSeq;    // ultimo seq eseguito (conferma comando)
};  // 30 byte

struct __attribute__((packed)) LoopMemEntry {
    char    name[LOOP_NAME_LEN];
    int32_t position;
};

struct __attribute__((packed)) LoopMemList {
    uint8_t magic;       // LOOP_MAGIC
    uint8_t type;        // LRSP_MEMLIST
    uint8_t proto;
    uint8_t count;       // LOOP_MEM_N
    LoopMemEntry mem[LOOP_MEM_N];
};  // 132 byte
