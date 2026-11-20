// OLR_LoopRX v2.0 - ricevitore ESP32 per Loop Magnetica
// Comando da Cardputer (ESP-NOW) oppure da cellulare (Access Point + pagina web).
// Tutti i dati (posizione, limiti, memorie, gioco) stanno SOLO qui, nella flash dell'ESP32:
// qualunque dispositivo usi, ritrovi le stesse tarature.
// --------------------------------------------------------------------
// Scheda: ESP32-WROOM-32 USB-C 38 pin (CP2102) - in Arduino IDE: "ESP32 Dev Module"
// Core:   esp32 di Espressif 2.x o 3.x
// Driver: ULN2003 + motore 28BYJ-48 (5 V)
//
// Cablaggio (lato destro della scheda, USB in alto):
//   pin 1  V5   -> ULN2003 "+"   (ESP32 alimentato da power bank via USB-C)
//   pin 6  GND  -> ULN2003 "-"
//   pin 8  G14  -> IN1
//   pin 9  G27  -> IN2
//   pin 10 G26  -> IN3
//   pin 11 G25  -> IN4
//   Ponticello di alimentazione del driver INSERITO.
//   Condensatore 470-1000 uF tra + e - del driver.
//   Pin 7 (G12) NON collegare: e' un pin di avvio.
//
// Uso:
//   - Dal telefono: rete "OLR_Loop" (password "loopmagn"), poi http://192.168.4.1
//   - Dal Cardputer: menu Loop -> "Cerca ricevitore" (abbinamento ESP-NOW)
//     L'abbinamento e' accettato: se la loop non e' ancora abbinata, nei primi
//     2 minuti dall'accensione, oppure dopo "Consenti abbinamento" dalla pagina web.
//   - Calibrazione: porta il condensatore al riferimento meccanico (es. tutto chiuso)
//     e premi "Zero qui". Poi imposta limiti minimo e massimo.

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "OLR_LoopProto.h"

#define FW_VERSION      200          // v2.00
#define FW_VERSION_STR  "2.0"

// ============ Pin motore ============
constexpr int MOTOR_PIN[4] = {14, 27, 26, 25};   // IN1..IN4
constexpr int LED_PIN = 2;
constexpr uint8_t HALF_STEP[8][4] = {
    {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
    {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
};

// ============ Impostazioni predefinite ============
constexpr int32_t DEF_MIN       = 0;
constexpr int32_t DEF_MAX       = 2048;   // mezzo giro dell'albero (variabile in aria tipico)
constexpr int     DEF_BACKLASH  = 40;     // passi di compensazione del gioco ingranaggi
constexpr int     DEF_STEP_MS   = 2;      // ms per mezzo passo (2 = ~7 giri/min)
constexpr int     DEF_CHANNEL   = 1;
constexpr unsigned long COIL_OFF_MS     = 1500;    // bobine spente dopo l'arrivo
constexpr unsigned long PAIR_WINDOW_MS  = 120000;  // finestra di abbinamento
constexpr unsigned long MASTER_IDLE_MS  = 30000;   // stato periodico solo se il Cardputer e' attivo

// ============ Stato ============
Preferences prefs;
WebServer http(80);

int32_t curPos = 0;          // posizione contata (passi)
int32_t targetPos = 0;       // prossima destinazione del motore
int32_t finalTarget = 0;     // destinazione finale (dopo l'eventuale sorpasso per il gioco)
bool    hasFinal = false;    // true = c'e' una seconda tappa (compensazione gioco)
int32_t minLimit = DEF_MIN, maxLimit = DEF_MAX;
int     backlash = DEF_BACKLASH;
int     stepMs = DEF_STEP_MS;
int     apChannel = DEF_CHANNEL;

int stepIdx = 0;
unsigned long lastStepMs = 0, arrivedMs = 0;
bool coilsEnergized = false;
bool wasMoving = false;

int32_t savedPos = 0;        // ultima posizione scritta in flash
bool    dirtyStored = false; // in flash: "movimento in corso"
bool    uncertain = false;   // all'avvio: spenta durante un movimento

struct Memory { char name[LOOP_NAME_LEN]; int32_t position; };
Memory memories[LOOP_MEM_N];
const char* defaultMemNames[LOOP_MEM_N] = {"40m","30m","20m","17m","15m","12m","11m","10m"};

char apSsid[33] = "OLR_Loop";
char apPass[64] = "loopmagn";

uint8_t masterMac[6] = {0};
bool paired = false;
uint32_t lastSeq = 0;
unsigned long pairWindowUntil = 0;
unsigned long lastMasterMs = 0;
bool masterSeen = false;
unsigned long lastStatusMs = 0, ledMs = 0;
bool ledState = false;
bool rebootPending = false;
unsigned long rebootAt = 0;

// Coda: la callback ESP-NOW copia i pacchetti qui, il loop() li esegue
struct RxItem { uint8_t mac[6]; uint8_t len; uint8_t data[32]; };
QueueHandle_t rxQueue = nullptr;

// ============ Utilita' ============
bool isMoving() { return curPos != targetPos || hasFinal; }
int32_t logicalTarget() { return hasFinal ? finalTarget : targetPos; }
int32_t clampPos(int32_t t) {
    if (t < minLimit) t = minLimit;
    if (t > maxLimit) t = maxLimit;
    return t;
}
void copyName(char* dst, const char* src) {
    size_t n = 0;
    while (src[n] && n < LOOP_NAME_LEN - 1) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

// ============ Motore ============
void setMotorPins(const uint8_t s[4]) { for (int i = 0; i < 4; i++) digitalWrite(MOTOR_PIN[i], s[i]); }
void energize(bool on) {
    coilsEnergized = on;
    if (on) setMotorPins(HALF_STEP[stepIdx]);
    else { const uint8_t off[4] = {0,0,0,0}; setMotorPins(off); }
}

void markDirty() {
    if (!dirtyStored) { prefs.putBool("dirty", true); dirtyStored = true; }
}

// Imposta una destinazione. Se si va verso il basso e la compensazione del gioco
// e' attiva, prima si supera il punto di "backlash" passi e poi si risale:
// cosi' l'arrivo e' sempre dalla stessa direzione e le memorie sono ripetibili.
void requestTarget(int32_t t) {
    t = clampPos(t);
    if (t == curPos && !isMoving()) return;
    if (backlash > 0 && t < curPos) {
        int32_t over = t - backlash;
        if (over < minLimit) over = minLimit;
        if (over < t) { targetPos = over; finalTarget = t; hasFinal = true; }
        else          { targetPos = t; hasFinal = false; }
    } else {
        targetPos = t; hasFinal = false;
    }
    markDirty();
}
void stopMotor() { targetPos = curPos; hasFinal = false; }

void saveSettledPosition() {
    if (curPos != savedPos) { prefs.putInt("pos", curPos); savedPos = curPos; }
    if (dirtyStored) { prefs.putBool("dirty", false); dirtyStored = false; }
}

// ============ Preferences ============
void saveLimits()  { prefs.putInt("min", minLimit); prefs.putInt("max", maxLimit); }
void saveMemory(int i) {
    char k[8];
    snprintf(k, sizeof(k), "n%d", i); prefs.putString(k, memories[i].name);
    snprintf(k, sizeof(k), "p%d", i); prefs.putInt(k, memories[i].position);
}
void savePairing() { prefs.putBytes("mac", masterMac, 6); prefs.putBool("paired", paired); }

void loadPrefs() {
    curPos   = prefs.getInt("pos", 0);
    minLimit = prefs.getInt("min", DEF_MIN);
    maxLimit = prefs.getInt("max", DEF_MAX);
    backlash = prefs.getInt("bl", DEF_BACKLASH);
    stepMs   = prefs.getInt("spd", DEF_STEP_MS);
    apChannel= prefs.getInt("ch", DEF_CHANNEL);
    if (stepMs < 1 || stepMs > 20) stepMs = DEF_STEP_MS;
    if (apChannel < 1 || apChannel > 13) apChannel = DEF_CHANNEL;
    if (backlash < 0 || backlash > 500) backlash = DEF_BACKLASH;
    if (maxLimit < minLimit) { int32_t t = minLimit; minLimit = maxLimit; maxLimit = t; }

    size_t sz = prefs.getBytes("mac", masterMac, 6);
    paired = prefs.getBool("paired", false) && sz == 6;

    uncertain   = prefs.getBool("dirty", false);
    dirtyStored = uncertain;
    savedPos  = curPos;
    targetPos = curPos;

    for (int i = 0; i < LOOP_MEM_N; i++) {
        char k[8];
        snprintf(k, sizeof(k), "n%d", i);
        String n = prefs.getString(k, defaultMemNames[i]);
        copyName(memories[i].name, n.c_str());
        snprintf(k, sizeof(k), "p%d", i);
        memories[i].position = prefs.getInt(k, 0);
    }
    String s = prefs.getString("ssid", apSsid);
    if (s.length() >= 1 && s.length() <= 32) { strncpy(apSsid, s.c_str(), sizeof(apSsid) - 1); apSsid[sizeof(apSsid) - 1] = 0; }
    String p = prefs.getString("pass", apPass);
    if (p.length() >= 8 && p.length() <= 63) { strncpy(apPass, p.c_str(), sizeof(apPass) - 1); apPass[sizeof(apPass) - 1] = 0; }
}

// ============ Operazioni comuni (web e ESP-NOW) ============
void setLimits(int32_t mn, int32_t mx) {
    if (mx < mn) { int32_t t = mn; mn = mx; mx = t; }
    minLimit = mn; maxLimit = mx;
    saveLimits();
    // Il limite agisce solo sulla destinazione: la posizione reale non viene toccata.
    if (isMoving()) requestTarget(logicalTarget());
}
void setZeroHere() {
    stopMotor();
    curPos = 0; targetPos = 0;
    uncertain = false;
    saveSettledPosition();
}
void setPositionHere(int32_t p) {
    stopMotor();
    curPos = p; targetPos = p;
    uncertain = false;
    saveSettledPosition();
}
void setMemory(int i, const char* name, int32_t pos) {
    if (i < 0 || i >= LOOP_MEM_N) return;
    if (name && name[0]) copyName(memories[i].name, name);
    memories[i].position = (pos == LOOP_POS_CURRENT) ? logicalTarget() : pos;
    saveMemory(i);
}
void gotoMemory(int i) {
    if (i < 0 || i >= LOOP_MEM_N) return;
    requestTarget(memories[i].position);
}
void setBacklash(int b) {
    if (b < 0) b = 0;
    if (b > 500) b = 500;
    backlash = b; prefs.putInt("bl", backlash);
}

// ============ ESP-NOW ============
void addPeer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = apChannel;
    p.ifidx = WIFI_IF_AP;      // la scheda e' solo Access Point: senza questo l'invio fallisce
    p.encrypt = false;
    esp_now_add_peer(&p);
}

void sendStatusTo(const uint8_t* mac) {
    LoopStatus st = {};
    st.magic = LOOP_MAGIC; st.type = LRSP_STATUS; st.proto = LOOP_PROTO;
    st.flags = (isMoving() ? LFLAG_MOVING : 0) | (uncertain ? LFLAG_UNCERTAIN : 0) | (paired ? LFLAG_PAIRED : 0);
    st.position = curPos;
    st.target = logicalTarget();
    st.minLimit = minLimit;
    st.maxLimit = maxLimit;
    st.backlash = (int16_t)backlash;
    st.fwVersion = FW_VERSION;
    st.lastSeq = lastSeq;
    esp_now_send(mac, (const uint8_t*)&st, sizeof(st));
}
void sendMemListTo(const uint8_t* mac) {
    LoopMemList ml = {};
    ml.magic = LOOP_MAGIC; ml.type = LRSP_MEMLIST; ml.proto = LOOP_PROTO; ml.count = LOOP_MEM_N;
    for (int i = 0; i < LOOP_MEM_N; i++) {
        copyName(ml.mem[i].name, memories[i].name);
        ml.mem[i].position = memories[i].position;
    }
    esp_now_send(mac, (const uint8_t*)&ml, sizeof(ml));
}
void sendStatus() { if (paired) sendStatusTo(masterMac); }

// Callback: gira nel task WiFi, quindi fa solo una copia nella coda.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    const uint8_t* mac = info->src_addr;
#else
void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
    if (!rxQueue || len <= 0 || len > 32) return;
    RxItem it;
    memcpy(it.mac, mac, 6);
    it.len = (uint8_t)len;
    memcpy(it.data, data, len);
    xQueueSend(rxQueue, &it, 0);
}

void handlePacket(const RxItem& it) {
    if (it.len != sizeof(LoopCmd)) return;
    LoopCmd c;
    memcpy(&c, it.data, sizeof(c));
    if (c.magic != LOOP_MAGIC || c.proto != LOOP_PROTO) return;
    c.name[LOOP_NAME_LEN - 1] = 0;

    bool fromMaster = paired && memcmp(it.mac, masterMac, 6) == 0;

    if (c.type == LCMD_PAIR) {
        bool allowed = !paired || fromMaster || millis() < pairWindowUntil;
        if (!allowed) return;
        if (!fromMaster) {
            if (paired) esp_now_del_peer(masterMac);
            memcpy(masterMac, it.mac, 6);
            paired = true;
            savePairing();
            Serial.printf("Abbinato a %02X:%02X:%02X:%02X:%02X:%02X\n",
                masterMac[0], masterMac[1], masterMac[2], masterMac[3], masterMac[4], masterMac[5]);
        }
        addPeer(masterMac);
        pairWindowUntil = 0;
        lastMasterMs = millis(); masterSeen = true;
        sendStatusTo(masterMac);
        sendMemListTo(masterMac);
        return;
    }
    if (!fromMaster) return;
    lastMasterMs = millis(); masterSeen = true;

    // Ritrasmissione dello stesso comando: non rieseguirlo, ma rispondi.
    bool duplicate = (c.seq != 0 && c.seq == lastSeq);
    bool sendMem = false;
    if (!duplicate) {
        lastSeq = c.seq;
        switch (c.type) {
            case LCMD_MOVE:     requestTarget(logicalTarget() + c.value); break;
            case LCMD_GOTO:     requestTarget(c.value); break;
            case LCMD_SET_ZERO: setZeroHere(); break;
            case LCMD_STOP:     stopMotor(); break;
            case LCMD_GET:      sendMem = true; break;
            case LCMD_SET_MIN:  setLimits(c.value, maxLimit); break;
            case LCMD_SET_MAX:  setLimits(minLimit, c.value); break;
            case LCMD_SET_POS:  setPositionHere(c.value); break;
            case LCMD_MEM_GO:   gotoMemory(c.idx); break;
            case LCMD_MEM_SET:  setMemory(c.idx, c.name, c.value); sendMem = true; break;
            case LCMD_MEM_LIST: sendMem = true; break;
            case LCMD_SET_BACKLASH: setBacklash(c.value); break;
            default: break;
        }
    } else if (c.type == LCMD_GET || c.type == LCMD_MEM_SET || c.type == LCMD_MEM_LIST) {
        sendMem = true;
    }
    sendStatusTo(masterMac);
    if (sendMem) sendMemListTo(masterMac);
}

// ============ Pagina web ============
const char PAGE_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>OLR Loop</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0 auto;padding:12px;font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:640px}
h1{margin:0 0 10px;font-size:18px;color:#8f8}
h1 small{color:#666;font-size:12px;font-weight:normal}
.card{background:#1b1b1b;border:1px solid #333;border-radius:8px;padding:12px;margin-bottom:10px}
#pos{font-size:72px;font-weight:bold;text-align:center;color:#8f8;line-height:1}
#posSub{text-align:center;color:#aaa;font-size:12px;margin-top:4px}
.bar{height:14px;background:#222;border:1px solid #444;border-radius:4px;overflow:hidden;margin-top:8px}
.fill{height:100%;background:#4a4;transition:width .3s}
.row{display:flex;gap:6px;margin-top:8px}
.row>*{flex:1}
button{background:#2a2a2a;color:#fff;border:1px solid #444;border-radius:6px;padding:14px 6px;font-size:16px;font-weight:bold;cursor:pointer;min-height:48px}
button:active{background:#444}
button.on{background:#4a4;color:#000}
button.red{background:#833}
button.sm{font-size:13px;padding:10px 4px;min-height:40px}
.mem{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
.mem button{padding:12px 2px;font-size:13px;display:flex;flex-direction:column;align-items:center}
.mem .n{font-size:16px}
.mem .p{font-size:10px;color:#aaa;margin-top:2px}
.mem button.cur{border-color:#4a4}
label{display:block;margin:8px 0 2px;font-size:12px;color:#aaa}
input{width:100%;background:#222;color:#fff;border:1px solid #444;border-radius:4px;padding:8px;font-size:14px}
.moving{color:#f80;font-size:13px;text-align:center;margin-top:6px}
.warn{background:#4a2a00;border-color:#a60;color:#fc8;font-size:13px}
.hidden{display:none}
summary{cursor:pointer;color:#aaa;font-size:13px}
.note{font-size:11px;color:#888;margin-top:4px}
</style></head><body>
<h1>OLR Loop Magnetica <small id="fw"></small></h1>

<div class="card warn hidden" id="unc">Posizione incerta: la scheda si e' spenta durante un movimento.
Riporta il condensatore al riferimento e premi "Zero qui" (in Calibrazione).</div>

<div class="card">
  <div id="pos">----</div>
  <div id="posSub"><span id="posMin">0</span> ... <span id="posMax">0</span></div>
  <div class="bar"><div class="fill" id="fill" style="width:0%"></div></div>
  <div class="moving hidden" id="moving">in movimento verso <span id="tgt"></span>...</div>
</div>

<div class="card">
  <div class="row">
    <button onclick="mv(-10)">&#9664;&#9664;</button>
    <button onclick="mv(-1)">&#9664;</button>
    <button class="red" onclick="cmd('stop')">&#9632;</button>
    <button onclick="mv(1)">&#9654;</button>
    <button onclick="mv(10)">&#9654;&#9654;</button>
  </div>
  <label>Passo:</label>
  <div class="row">
    <button class="sm" id="s1" onclick="setStep(1)">1</button>
    <button class="sm" id="s5" onclick="setStep(5)">5</button>
    <button class="sm" id="s20" onclick="setStep(20)">20</button>
    <button class="sm" id="s100" onclick="setStep(100)">100</button>
  </div>
  <label>Vai alla posizione:</label>
  <div class="row"><input type="number" id="gto" inputmode="numeric"><button class="sm" onclick="goTo()">Vai</button></div>
</div>

<div class="card">
  <label>Memorie (tocca = vai, tieni premuto = salva/modifica)</label>
  <div class="mem" id="mem"></div>
</div>

<details class="card"><summary>Calibrazione e limiti</summary>
  <div class="row">
    <button class="sm" onclick="if(confirm('La posizione attuale diventa 0?'))cmd('zero')">Zero qui</button>
    <button class="sm" onclick="setPos()">Imposta posizione</button>
  </div>
  <label>Limite minimo:</label>
  <div class="row"><input type="number" id="lmin"><button class="sm" onclick="setLim('min')">OK</button><button class="sm" onclick="limHere('min')">= attuale</button></div>
  <label>Limite massimo:</label>
  <div class="row"><input type="number" id="lmax"><button class="sm" onclick="setLim('max')">OK</button><button class="sm" onclick="limHere('max')">= attuale</button></div>
  <div class="note">I limiti fermano solo le destinazioni: non spostano la posizione contata.</div>
</details>

<details class="card"><summary>Motore</summary>
  <label>Compensazione gioco (passi, 0 = spenta):</label>
  <div class="row"><input type="number" id="bl" min="0" max="500"><button class="sm" onclick="setCfg('bl')">OK</button></div>
  <div class="note">Scendendo, il motore supera il punto di questi passi e poi risale: l'arrivo e' sempre dallo stesso lato.</div>
  <label>Velocita' (ms per passo, 1 = veloce, 10 = lento):</label>
  <div class="row"><input type="number" id="spd" min="1" max="20"><button class="sm" onclick="setCfg('spd')">OK</button></div>
</details>

<details class="card"><summary>Cardputer (ESP-NOW)</summary>
  <div id="pairInfo" class="note"></div>
  <div class="row">
    <button class="sm" onclick="cmd('pairopen')">Consenti abbinamento (2 min)</button>
    <button class="sm" onclick="if(confirm('Dimenticare il Cardputer abbinato?'))cmd('unpair')">Dimentica</button>
  </div>
</details>

<details class="card"><summary>Rete WiFi (serve riavvio)</summary>
  <label>Nome rete (SSID):</label><input type="text" id="apS" maxlength="32">
  <label>Nuova password (min 8, vuoto = invariata):</label><input type="text" id="apP" maxlength="63">
  <label>Canale (1-13, deve coincidere con quello del Cardputer):</label><input type="number" id="ch" min="1" max="13">
  <div class="row"><button class="sm" onclick="setWifi()">Salva e riavvia</button></div>
</details>

<script>
let step=5,S=null,editing=false;
const $=id=>document.getElementById(id);
function setStep(n){step=n;for(const s of [1,5,20,100])$('s'+s).classList.toggle('on',s==n)}
setStep(5);
function api(u){return fetch(u).then(r=>r.text())}
function mv(m){api('/api/move?n='+(m*step)).then(poll)}
function cmd(c){api('/api/'+c).then(poll)}
function goTo(){const v=$('gto').value;if(v!=='')api('/api/goto?p='+v).then(poll)}
function setPos(){const v=prompt('Posizione reale attuale (passi):',S?S.pos:0);if(v!==null&&v!=='')api('/api/setpos?p='+parseInt(v)).then(poll)}
function setLim(w){const v=$('l'+w).value;if(v!=='')api('/api/limit?'+w+'='+v).then(poll)}
function limHere(w){if(S)api('/api/limit?'+w+'='+S.pos).then(poll)}
function setCfg(k){const v=$(k).value;if(v!=='')api('/api/cfg?'+k+'='+v).then(poll)}
function setWifi(){
  const s=$('apS').value.trim(),p=$('apP').value,c=$('ch').value;
  if(s.length<1){alert('Nome rete vuoto');return}
  if(p.length>0&&p.length<8){alert('Password: minimo 8 caratteri');return}
  if(!confirm('Salvare e riavviare? Ricollegati poi alla rete "'+s+'".'))return;
  api('/api/wifi?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)+'&ch='+c).then(()=>alert('Salvato, la scheda si riavvia'));
}
function memGo(i){api('/api/mem/go?i='+i).then(poll)}
function memEdit(i){
  editing=true;
  const name=prompt('Nome memoria '+(i+1)+' (max 11 caratteri):',S.memN[i]);
  if(name===null){editing=false;return}
  const p=prompt('Posizione (vuoto = posizione attuale '+S.pos+'):','');
  editing=false;
  if(p===null)return;
  let url='/api/mem/set?i='+i+'&n='+encodeURIComponent(name.slice(0,11));
  if(p!=='')url+='&p='+parseInt(p);
  api(url).then(poll);
}
let pt=null;
function down(i){pt=setTimeout(()=>{pt=null;memEdit(i)},600)}
function up(i){if(pt){clearTimeout(pt);pt=null;memGo(i)}}
function cancel(){if(pt){clearTimeout(pt);pt=null}}
function buildMem(){
  const box=$('mem');box.innerHTML='';
  for(let i=0;i<8;i++){
    const b=document.createElement('button');
    b.id='m'+i;
    b.onpointerdown=()=>down(i);b.onpointerup=()=>up(i);b.onpointerleave=cancel;
    b.oncontextmenu=e=>e.preventDefault();
    const n=document.createElement('span');n.className='n';
    const p=document.createElement('span');p.className='p';
    b.appendChild(n);b.appendChild(p);box.appendChild(b);
  }
}
buildMem();
function poll(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    S=s;
    $('fw').textContent='v'+s.fw;
    $('pos').textContent=s.pos;
    $('posMin').textContent=s.min;$('posMax').textContent=s.max;
    const pct=(s.max>s.min)?(100*(s.pos-s.min)/(s.max-s.min)):0;
    $('fill').style.width=Math.max(0,Math.min(100,pct))+'%';
    $('moving').classList.toggle('hidden',!s.moving);$('tgt').textContent=s.tgt;
    $('unc').classList.toggle('hidden',!s.unc);
    for(let i=0;i<8;i++){const b=$('m'+i);b.children[0].textContent=s.memN[i];b.children[1].textContent=s.memP[i];
      b.classList.toggle('cur',!s.moving&&s.pos==s.memP[i])}
    const a=document.activeElement;
    const set=(id,v)=>{if(a!==$(id))$(id).value=v};
    set('lmin',s.min);set('lmax',s.max);set('bl',s.bl);set('spd',s.spd);set('ch',s.ch);set('apS',s.ssid);
    $('pairInfo').textContent=(s.paired?'Abbinato a '+s.mac:'Nessun Cardputer abbinato')+(s.pairOpen?' - abbinamento APERTO':'');
  }).catch(()=>{});
}
setInterval(()=>{if(!editing)poll()},500);poll();
</script></body></html>
)HTML";

// ============ Handlers HTTP ============
String jesc(const char* s) {
    String r;
    for (const char* p = s; *p; p++) {
        if (*p == '"' || *p == '\\') { r += '\\'; r += *p; }
        else if ((uint8_t)*p < 32) r += ' ';
        else r += *p;
    }
    return r;
}
void okReply() { http.send(200, "text/plain", "OK"); }

void hStatus() {
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             masterMac[0], masterMac[1], masterMac[2], masterMac[3], masterMac[4], masterMac[5]);
    String j; j.reserve(700);
    j  = "{\"fw\":\"" FW_VERSION_STR "\"";
    j += ",\"pos\":" + String((long)curPos);
    j += ",\"tgt\":" + String((long)logicalTarget());
    j += ",\"min\":" + String((long)minLimit);
    j += ",\"max\":" + String((long)maxLimit);
    j += ",\"moving\":"; j += isMoving() ? "true" : "false";
    j += ",\"unc\":";    j += uncertain ? "true" : "false";
    j += ",\"bl\":" + String(backlash);
    j += ",\"spd\":" + String(stepMs);
    j += ",\"ch\":" + String(apChannel);
    j += ",\"ssid\":\"" + jesc(apSsid) + "\"";
    j += ",\"paired\":"; j += paired ? "true" : "false";
    j += ",\"mac\":\""; j += mac; j += "\"";
    j += ",\"pairOpen\":"; j += (millis() < pairWindowUntil) ? "true" : "false";
    j += ",\"memN\":[";
    for (int i = 0; i < LOOP_MEM_N; i++) { if (i) j += ","; j += "\"" + jesc(memories[i].name) + "\""; }
    j += "],\"memP\":[";
    for (int i = 0; i < LOOP_MEM_N; i++) { if (i) j += ","; j += String((long)memories[i].position); }
    j += "]}";
    http.send(200, "application/json", j);
}
void hRoot()  { http.send_P(200, "text/html; charset=utf-8", PAGE_HTML); }
void hMove()  { requestTarget(logicalTarget() + http.arg("n").toInt()); okReply(); }
void hGoto()  { requestTarget(http.arg("p").toInt()); okReply(); }
void hStop()  { stopMotor(); okReply(); }
void hZero()  { setZeroHere(); okReply(); }
void hSetPos(){ if (http.hasArg("p")) setPositionHere(http.arg("p").toInt()); okReply(); }
void hMemGo() {
    int i = http.arg("i").toInt();
    if (i < 0 || i >= LOOP_MEM_N) { http.send(400, "text/plain", "bad"); return; }
    gotoMemory(i); okReply();
}
void hMemSet() {
    int i = http.arg("i").toInt();
    if (i < 0 || i >= LOOP_MEM_N) { http.send(400, "text/plain", "bad"); return; }
    String n = http.hasArg("n") ? http.arg("n") : String("");
    n.trim();
    int32_t p = http.hasArg("p") ? (int32_t)http.arg("p").toInt() : LOOP_POS_CURRENT;
    setMemory(i, n.c_str(), p);
    okReply();
}
void hLimit() {
    int32_t mn = http.hasArg("min") ? (int32_t)http.arg("min").toInt() : minLimit;
    int32_t mx = http.hasArg("max") ? (int32_t)http.arg("max").toInt() : maxLimit;
    setLimits(mn, mx); okReply();
}
void hCfg() {
    if (http.hasArg("bl")) setBacklash(http.arg("bl").toInt());
    if (http.hasArg("spd")) {
        int v = http.arg("spd").toInt();
        if (v >= 1 && v <= 20) { stepMs = v; prefs.putInt("spd", stepMs); }
    }
    okReply();
}
void hPairOpen() { pairWindowUntil = millis() + PAIR_WINDOW_MS; okReply(); }
void hUnpair() {
    if (paired) esp_now_del_peer(masterMac);
    paired = false; memset(masterMac, 0, 6); masterSeen = false;
    savePairing(); okReply();
}
void hWifi() {
    String s = http.arg("ssid"); s.trim();
    String p = http.arg("pass");
    int ch = http.hasArg("ch") ? http.arg("ch").toInt() : apChannel;
    if (s.length() < 1 || s.length() > 32) { http.send(400, "text/plain", "SSID non valido"); return; }
    if (p.length() > 0 && (p.length() < 8 || p.length() > 63)) { http.send(400, "text/plain", "Password non valida"); return; }
    if (ch < 1 || ch > 13) ch = apChannel;
    strncpy(apSsid, s.c_str(), sizeof(apSsid) - 1); apSsid[sizeof(apSsid) - 1] = 0;
    prefs.putString("ssid", apSsid);
    if (p.length() > 0) { strncpy(apPass, p.c_str(), sizeof(apPass) - 1); apPass[sizeof(apPass) - 1] = 0; prefs.putString("pass", apPass); }
    apChannel = ch; prefs.putInt("ch", apChannel);
    http.send(200, "text/plain", "REBOOT");
    stopMotor();
    rebootPending = true; rebootAt = millis() + 800;
}

// ============ Setup / Loop ============
void setup() {
    // Prima di tutto: uscite motore a 0
    for (int i = 0; i < 4; i++) { pinMode(MOTOR_PIN[i], OUTPUT); digitalWrite(MOTOR_PIN[i], 0); }
    pinMode(LED_PIN, OUTPUT);

    Serial.begin(115200); delay(200);
    Serial.println("\nOLR LoopRX v" FW_VERSION_STR " avvio");

    prefs.begin("looprx2", false);
    loadPrefs();

    rxQueue = xQueueCreate(8, sizeof(RxItem));

    // AP ed ESP-NOW usano lo stesso canale
    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSsid, apPass, apChannel, 0, 4);
    Serial.printf("AP: %s  canale %d  IP: %s\n", apSsid, apChannel, WiFi.softAPIP().toString().c_str());
    Serial.print("MAC (AP): "); Serial.println(WiFi.softAPmacAddress());

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init KO");
    } else {
        esp_now_register_recv_cb(onRecv);
        uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        addPeer(bcast);
        if (paired) addPeer(masterMac);
    }
    pairWindowUntil = millis() + PAIR_WINDOW_MS;

    http.on("/", hRoot);
    http.on("/api/status", hStatus);
    http.on("/api/move", hMove);
    http.on("/api/goto", hGoto);
    http.on("/api/stop", hStop);
    http.on("/api/zero", hZero);
    http.on("/api/setpos", hSetPos);
    http.on("/api/mem/go", hMemGo);
    http.on("/api/mem/set", hMemSet);
    http.on("/api/limit", hLimit);
    http.on("/api/cfg", hCfg);
    http.on("/api/pairopen", hPairOpen);
    http.on("/api/unpair", hUnpair);
    http.on("/api/wifi", hWifi);
    http.onNotFound([]() { http.sendHeader("Location", "/"); http.send(302, "text/plain", ""); });
    http.begin();

    Serial.printf("Pos=%ld min=%ld max=%ld gioco=%d abbinato=%d%s\n",
                  (long)curPos, (long)minLimit, (long)maxLimit, backlash, paired ? 1 : 0,
                  uncertain ? "  ATTENZIONE: posizione incerta" : "");
}

void loop() {
    http.handleClient();

    RxItem it;
    while (rxQueue && xQueueReceive(rxQueue, &it, 0) == pdTRUE) handlePacket(it);

    unsigned long now = millis();

    // --- Motore ---
    if (curPos != targetPos && now - lastStepMs >= (unsigned long)stepMs) {
        lastStepMs = now;
        if (!coilsEnergized) energize(true);
        int dir = (targetPos > curPos) ? 1 : -1;
        stepIdx = (stepIdx + 8 + dir) % 8;
        setMotorPins(HALF_STEP[stepIdx]);
        curPos += dir;
    }
    if (curPos == targetPos && hasFinal) {        // seconda tappa della compensazione gioco
        targetPos = finalTarget; hasFinal = false;
    }
    bool moving = isMoving();
    if (moving) {
        wasMoving = true;
    } else if (wasMoving) {                        // appena arrivato
        wasMoving = false;
        arrivedMs = now;
        sendStatus();
    }
    if (!moving && coilsEnergized && now - arrivedMs > COIL_OFF_MS) {
        energize(false);
        saveSettledPosition();                     // posizione salvata a motore fermo
    }

    // --- Stato periodico al Cardputer (solo se e' attivo) ---
    if (paired && masterSeen && now - lastMasterMs < MASTER_IDLE_MS && now - lastStatusMs > 1000) {
        lastStatusMs = now; sendStatus();
    }

    // --- LED: veloce = movimento, medio = abbinamento aperto, lento = normale ---
    unsigned long blinkT = moving ? 100 : (now < pairWindowUntil ? 400 : 1500);
    if (now - ledMs > blinkT) { ledMs = now; ledState = !ledState; digitalWrite(LED_PIN, ledState); }

    if (rebootPending && now > rebootAt) {
        energize(false);
        saveSettledPosition();
        ESP.restart();
    }
}
