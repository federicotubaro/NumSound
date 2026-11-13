/*
 * NumSound - Harmonic Sequence Synthesizer & Stereo Panorama (v1.0 C++ Port)
 * Copyright (c) 2026 Federico Tubaro - All Rights Reserved.
 */

#include "raylib.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOGDI
    #define NOUSER
    #define NOMINMAX
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Alias sicuri per evitare problemi di formattazione
typedef std::vector<double> VecDouble;
typedef std::vector<int> VecInt;
typedef std::lock_guard<std::mutex> MutexLock;
typedef std::atomic<bool> AtomicBool;
typedef std::atomic<double> AtomicDouble;
typedef std::atomic<int> AtomicInt;

// --- INCLUDE MIDI NATIVI PER WINDOWS E MACOS ---
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOGDI
    #define NOUSER
    #include <windows.h>
    #include <mmsystem.h>
    #pragma comment(lib, "winmm.lib")
#elif defined(__APPLE__)
    #include <CoreMIDI/CoreMIDI.h>
#endif

// --- COSTANTI E STATO GLOBALE ---
constexpr float SAMPLE_RATE = 44100.0f;
constexpr int BUFFER_SAMPLES = 131072;
constexpr int MAX_OSCI = 256;

AtomicBool mostraOndeSingole{true};
AtomicDouble cutoffLP{16000.0};
AtomicInt formaOnda{0};
AtomicBool isPlaying{false};
AtomicBool triggerAttivo{true};
AtomicDouble sfasamentoNorm{0.0};
AtomicBool resetFasiRichiesto{false};
AtomicDouble frequenzaFondamentale{64.0};

std::mutex seqMutex;
VecDouble frequenzeAttive;
VecInt cifreAttive;
std::string sequenzaCorrente = "";

std::mutex snapMutex;
VecDouble latestSnapshot(BUFFER_SAMPLES, 0.0);

AtomicBool midiNotePending{false};
AtomicDouble midiPendingFreq{64.0};
AtomicBool midiCCPending{false};
AtomicInt midiPendingSliderVal{1000};

const double RAPPORTI_PITAGORICI[12] = {
    1.0, 256.0 / 243.0, 9.0 / 8.0, 32.0 / 27.0,
    81.0 / 64.0, 4.0 / 3.0, 729.0 / 512.0, 3.0 / 2.0,
    128.0 / 81.0, 27.0 / 16.0, 16.0 / 9.0, 243.0 / 128.0
};

const char* NOMI_NOTE[12] = {
    "DO", "REb", "RE", "MIb", "MI", "FA", "FA#", "SOL", "LAb", "LA", "SIb", "SI"
};

// --- FUNZIONI MUSICALI E COLORI ---
std::string calcolaNotaPitagorica(double freq) {
    if (freq <= 0.0) return "Nota: --";
    double refDo2 = 128.0;
    int ottava = 2;
    double freqNorm = freq;
    while (freqNorm < refDo2) { freqNorm *= 2.0; ottava--; }
    while (freqNorm >= refDo2 * 2.0) { freqNorm /= 2.0; ottava++; }

    double rapportoCorrente = freqNorm / refDo2;
    int indiceMigliore = 0;
    double minDiffCents = 1e9;
    double freqNotaEsatta = refDo2;

    for (int i = 0; i < 12; i++) {
        double r = RAPPORTI_PITAGORICI[i];
        double diffCents = 1200.0 * std::abs(std::log(rapportoCorrente / r) / std::log(2.0));
        if (diffCents < minDiffCents) {
            minDiffCents = diffCents;
            indiceMigliore = i;
            freqNotaEsatta = r * 128.0 * std::pow(2.0, ottava - 2);
        }
    }
    double diffDoSup = 1200.0 * std::abs(std::log(rapportoCorrente / 2.0) / std::log(2.0));
    if (diffDoSup < minDiffCents) {
        minDiffCents = diffDoSup;
        indiceMigliore = 0;
        ottava++;
        freqNotaEsatta = 128.0 * std::pow(2.0, ottava - 2);
    }

    char buf[64];
    if (minDiffCents < 1.5) {
        std::snprintf(buf, sizeof(buf), "Nota: %s%d (%.1f Hz)", NOMI_NOTE[indiceMigliore], ottava, freqNotaEsatta);
    } else {
        std::snprintf(buf, sizeof(buf), "Nota: ~%s%d (%.1f Hz)", NOMI_NOTE[indiceMigliore], ottava, freqNotaEsatta);
    }
    return std::string(buf);
}

Color calcolaColoreDaFrequenza(double freq, unsigned char alpha = 255) {
    if (freq <= 0.0) return GRAY;
    double semitoniDaDo1 = 12.0 * (std::log(freq / 64.0) / std::log(2.0));
    double posOttava = std::fmod((std::fmod(semitoniDaDo1, 12.0) + 12.0), 12.0) / 12.0;

    float hue = (float)std::fmod(0.36 + posOttava * 1.12, 1.0) * 360.0f;
    double ottava = semitoniDaDo1 / 12.0;
    float brightness = (float)std::max(0.35, std::min(1.0, 0.45 + ottava * 0.16));
    float saturation = (float)std::max(0.45, std::min(0.95, 0.95 - std::max(0.0, ottava - 2.5) * 0.18));

    Color c = ColorFromHSV(hue, saturation, brightness);
    c.a = alpha;
    return c;
}

void aggiornaDatiSequenza(const std::string& testoPulito, double nuovaBase) {
    VecDouble nuoveFreq;
    VecInt nuoveCifre;
    std::string seqPulita;

    int limit = std::min((int)testoPulito.size(), MAX_OSCI);
    for (int i = 0; i < limit; i++) {
        int cifra = testoPulito[i] - '0';
        if (cifra >= 1 && cifra <= 9) {
            nuoveCifre.push_back(cifra);
            nuoveFreq.push_back(cifra * nuovaBase);
            seqPulita += std::to_string(cifra) + " ";
        }
    }
    if (!seqPulita.empty()) seqPulita.pop_back();

    MutexLock lock(seqMutex);
    frequenzaFondamentale.store(nuovaBase);
    cifreAttive = std::move(nuoveCifre);
    frequenzeAttive = std::move(nuoveFreq);
    sequenzaCorrente = seqPulita;
}

// --- CALLBACK AUDIO REAL-TIME ---
void audioCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pDevice; (void)pInput;
    float* out = (float*)pOutput;

    static double fasi[MAX_OSCI] = {0.0};
    static int prevNumOsci = 0;
    static double envelope = 0.0;
    static double lpLeft1 = 0.0, lpLeft2 = 0.0;
    static double lpRight1 = 0.0, lpRight2 = 0.0;
    static VecDouble rollingHistory(BUFFER_SAMPLES, 0.0);
    static int contatoreGrafica = 0;

    double localFreqs[MAX_OSCI];
    int numOsci = 0;
    {
        MutexLock lock(seqMutex);
        numOsci = std::min((int)frequenzeAttive.size(), MAX_OSCI);
        for (int i = 0; i < numOsci; i++) localFreqs[i] = frequenzeAttive[i];
    }

    if (numOsci != prevNumOsci) {
        for (int i = prevNumOsci; i < numOsci; i++) fasi[i] = 0.0;
        prevNumOsci = numOsci;
    }

    if (resetFasiRichiesto.exchange(false)) {
        std::memset(fasi, 0, sizeof(fasi));
    }

    bool playing = isPlaying.load();
    if ((!playing && envelope <= 0.0) || numOsci == 0) {
        std::memset(out, 0, frameCount * 2 * sizeof(float));
        return;
    }

    double gainLeft[MAX_OSCI];
    double gainRight[MAX_OSCI];
    for (int k = 0; k < numOsci; k++) {
        double angolo = (numOsci == 1) ? (M_PI / 4.0) : (k * (M_PI / 2.0) / (numOsci - 1));
        gainLeft[k] = std::cos(angolo);
        gainRight[k] = std::sin(angolo);
    }

    int tipoOnda = formaOnda.load();
    double cutoff = cutoffLP.load();
    double sfasamento = sfasamentoNorm.load();
    double fadeStep = 1.0 / (SAMPLE_RATE * 0.08);

    double alpha = 0.0;
    if (cutoff > 0.0) {
        double dt = 1.0 / SAMPLE_RATE;
        double rc = 1.0 / (2.0 * M_PI * cutoff);
        alpha = dt / (rc + dt);
    }

    if (frameCount < BUFFER_SAMPLES) {
        std::memmove(rollingHistory.data(), rollingHistory.data() + frameCount, (BUFFER_SAMPLES - frameCount) * sizeof(double));
    }

    double fattoreNorm = std::sqrt((double)numOsci);

    for (ma_uint32 i = 0; i < frameCount; i++) {
        if (playing && envelope < 1.0) envelope = std::min(1.0, envelope + fadeStep);
        else if (!playing && envelope > 0.0) envelope = std::max(0.0, envelope - fadeStep);

        double sommaLeft = 0.0;
        double sommaRight = 0.0;

        for (int k = 0; k < numOsci; k++) {
            double fase = fasi[k];
            double distCentro = k - (numOsci - 1) / 2.0;
            double faseEffettiva = fase - (distCentro * sfasamento);
            faseEffettiva -= std::floor(faseEffettiva);

            double onda;
            if (tipoOnda == 1) onda = 1.0 - 4.0 * std::abs(faseEffettiva - 0.5);
            else if (tipoOnda == 2) onda = (faseEffettiva < 0.5) ? 1.0 : -1.0;
            else if (tipoOnda == 3) onda = (2.0 * faseEffettiva) - 1.0;
            else onda = std::sin(2.0 * M_PI * faseEffettiva);

            sommaLeft += onda * gainLeft[k];
            sommaRight += onda * gainRight[k];

            fase += localFreqs[k] / SAMPLE_RATE;
            if (fase >= 1.0) fase -= std::floor(fase);
            fasi[k] = fase;
        }

        lpLeft1 += alpha * (sommaLeft - lpLeft1);
        lpLeft2 += alpha * (lpLeft1 - lpLeft2);
        sommaLeft = lpLeft2;

        lpRight1 += alpha * (sommaRight - lpRight1);
        lpRight2 += alpha * (lpRight1 - lpRight2);
        sommaRight = lpRight2;

        double normLeft = std::tanh((sommaLeft / fattoreNorm) * 0.65) * 0.65 * envelope;
        double normRight = std::tanh((sommaRight / fattoreNorm) * 0.65) * 0.65 * envelope;

        out[i * 2] = (float)normLeft;
        out[i * 2 + 1] = (float)normRight;

        if (frameCount < BUFFER_SAMPLES) {
            rollingHistory[(BUFFER_SAMPLES - frameCount) + i] = ((normLeft + normRight) * 0.5) * 2.0;
        }
    }

    if (++contatoreGrafica % 4 == 0) {
        if (snapMutex.try_lock()) {
            latestSnapshot = rollingHistory;
            snapMutex.unlock();
        }
    }
}

// --- GESTIONE MIDI NATIVA (WINDOWS E MACOS) ---
void processMidiBytes(unsigned char status, unsigned char data1, unsigned char data2) {
    unsigned char cmd = status & 0xF0;
    if (cmd == 0x90 && data2 > 0) {
        int notaMidi = data1;
        int diffDaDoCentrale = notaMidi - 60;
        int ottave = (int)std::floor(diffDaDoCentrale / 12.0);
        int indiceNota = ((diffDaDoCentrale % 12) + 12) % 12;
        double freq = 256.0 * std::pow(2.0, ottave) * RAPPORTI_PITAGORICI[indiceNota];

        midiPendingFreq.store(freq);
        midiNotePending.store(true);
    } else if (cmd == 0xB0 && data1 == 7) {
        int sliderVal = (int)std::round((data2 / 127.0) * 1000.0);
        midiPendingSliderVal.store(sliderVal);
        midiCCPending.store(true);
    }
}

#ifdef _WIN32
std::vector<HMIDIIN> midiHandles;
void CALLBACK winMidiCallback(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {

(void)hMidiIn; (void)dwInstance; (void)dwParam2;
if (wMsg == MIM_DATA) {
unsigned char status = dwParam1 & 0xFF;
unsigned char data1 = (dwParam1 >> 8) & 0xFF;
unsigned char data2 = (dwParam1 >> 16) & 0xFF;
processMidiBytes(status, data1, data2);
}
}
void initMidi() {
UINT numDevs = midiInGetNumDevs();
for (UINT i = 0; i < numDevs; i++) {
HMIDIIN hMidi;
if (midiInOpen(&hMidi, i, (DWORD_PTR)winMidiCallback, 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
midiInStart(hMidi);
midiHandles.push_back(hMidi);
}
}
}
void closeMidi() {
for (auto h : midiHandles) { midiInStop(h); midiInClose(h); }
}
#elif defined(__APPLE__)
	MIDIClientRef midiClient = 0;
MIDIPortRef midiInPort = 0;

void macMidiReadProc(const MIDIPacketList* pktlist, void* readProcRefCon, void* srcConnRefCon) {
    (void)readProcRefCon; (void)srcConnRefCon;
    const MIDIPacket* packet = pktlist->packet;
    for (UInt32 i = 0; i < pktlist->numPackets; ++i) {
        const unsigned char* d = packet->data;
        UInt16 len = packet->length;
        UInt16 idx = 0;
        while (idx + 2 < len) {
            unsigned char status = *(d + idx);
            if (status & 0x80) {
                unsigned char d1 = *(d + idx + 1);
                unsigned char d2 = *(d + idx + 2);
                processMidiBytes(status, d1, d2);
                idx += 3;
            } else {
                idx++;
            }
        }
        packet = MIDIPacketNext(packet);
    }
}

void initMidi() {
MIDIClientCreate(CFSTR("Num2SoundMIDI"), nullptr, nullptr, &midiClient);
MIDIInputPortCreate(midiClient, CFSTR("InputPort"), macMidiReadProc, nullptr, &midiInPort);
ItemCount numSources = MIDIGetNumberOfSources();
for (ItemCount i = 0; i < numSources; ++i) {
MIDIEndpointRef src = MIDIGetSource(i);
MIDIPortConnectSource(midiInPort, src, nullptr);
}
}
void closeMidi() {
if (midiInPort) MIDIPortDispose(midiInPort);
if (midiClient) MIDIClientDispose(midiClient);
}
#else
void initMidi() {}
void closeMidi() {}
#endif
// --- HELPER UI RAYLIB 5.5 ---
bool DrawButton(Rectangle rect, const char* text, Color bg, Color hoverBg, Color textColor, int fontSize = 16) {
Vector2 mouse = GetMousePosition();
bool hover = CheckCollisionPointRec(mouse, rect);
DrawRectangleRounded(rect, 0.25f, 8, hover ? hoverBg : bg);
DrawRectangleRoundedLines(rect, 0.25f, 8, 1.5f, Color{70, 90, 110, 255});
int textW = MeasureText(text, fontSize);
DrawText(text, (int)(rect.x + (rect.width - textW) / 2), (int)(rect.y + (rect.height - fontSize) / 2), fontSize, textColor);
return hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}
void avviaOAggiorna(const std::string& txtSequenza, const std::string& txtFondamentale, std::string& msgErrore, float& msgErroreTimer) {
double nuovaBase = std::atof(txtFondamentale.c_str());
if (nuovaBase < 1.0 || nuovaBase > 4096.0) {
msgErrore = "Inserisci una frequenza fondamentale valida (tra 1 e 4096 Hz).";
msgErroreTimer = 3.0f;
return;
}
std::string pulita;
for (char c : txtSequenza) {
if (c >= '1' && c <= '9') pulita += c;
}
if (pulita.empty()) {
msgErrore = "Inserisci almeno una cifra valida compresa tra 1 e 9.";
msgErroreTimer = 3.0f;
return;
}
aggiornaDatiSequenza(pulita, nuovaBase);
isPlaying.store(true);
}
// --- MAIN ---
int main() {
SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
InitWindow(1160, 680, "Num2Sound — v. 1.0 C++ - (c)2026 Federico Tubaro");
SetTargetFPS(60);
ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
deviceConfig.playback.format = ma_format_f32;
deviceConfig.playback.channels = 2;
deviceConfig.sampleRate = (ma_uint32)SAMPLE_RATE;
deviceConfig.periodSizeInFrames = 128;
deviceConfig.dataCallback = audioCallback;
ma_device audioDevice;
if (ma_device_init(nullptr, &deviceConfig, &audioDevice) != MA_SUCCESS) {
std::printf("Errore inizializzazione scheda audio!\n");
return -1;
}
ma_device_start(&audioDevice);
initMidi();
std::string txtSequenza = "";
std::string txtFondamentale = "64";
int activeTextBox = 1;
int sliderLPVal = 1000;
int sliderFaseVal = 0;
std::string msgErrore = "";
float msgErroreTimer = 0.0f;
const char* tipiOnda[4] = {"Sinusoide", "Triangolare", "Onda Quadra", "Dente di Sega"};
VecDouble localSnapshot(BUFFER_SAMPLES, 0.0);
while (!WindowShouldClose()) {
float dt = GetFrameTime();
if (msgErroreTimer > 0.0f) msgErroreTimer -= dt;
if (midiNotePending.exchange(false)) {
double freq = midiPendingFreq.load();
char buf[32];
std::snprintf(buf, sizeof(buf), "%.2f", freq);
txtFondamentale = buf;
std::string pulita;
for (char c : txtSequenza) if (c >= '1' && c <= '9') pulita += c;
if (!pulita.empty()) {
aggiornaDatiSequenza(pulita, freq);
if (!isPlaying.load()) isPlaying.store(true);
}
}
if (midiCCPending.exchange(false)) {
sliderLPVal = midiPendingSliderVal.load();
if (sliderLPVal == 0) cutoffLP.store(0.0);
else cutoffLP.store(10.0 * std::pow(16000.0 / 10.0, sliderLPVal / 1000.0));
}
int key = GetCharPressed();
while (key > 0) {
if (activeTextBox == 1 && key >= '1' && key <= '9' && txtSequenza.size() < 64) {
txtSequenza += (char)key;
} else if (activeTextBox == 2 && ((key >= '0' && key <= '9') || key == '.' || key == ',') && txtFondamentale.size() < 12) {
txtFondamentale += (key == ',') ? '.' : (char)key;
}
key = GetCharPressed();
}
if (IsKeyPressed(KEY_BACKSPACE)) {
if (activeTextBox == 1 && !txtSequenza.empty()) txtSequenza.pop_back();
else if (activeTextBox == 2 && !txtFondamentale.empty()) txtFondamentale.pop_back();
}
if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
avviaOAggiorna(txtSequenza, txtFondamentale, msgErrore, msgErroreTimer);
}
int w = GetScreenWidth();
int h = GetScreenHeight();
int topBarH = 60;
int botBarH = 60;
int centerH = h - topBarH - botBarH;
int altezzaSpettro = std::max(170, (int)(centerH * 0.35f));
int altezzaOnda = centerH - altezzaSpettro;
int midY = topBarH + altezzaOnda / 2;
BeginDrawing();
ClearBackground(BLACK);
// --- 1. OSCILLOSCOPIO ---
for (int i = 1; i < 16; i++) {
int x = (int)std::round((i / 16.0) * w);
DrawLine(x, topBarH, x, topBarH + altezzaOnda, (i == 8) ? Color{48, 64, 82, 255} : Color{32, 42, 54, 255});
}
double ampMaxPixel = altezzaOnda * 0.42;
for (int i = 1; i <= 4; i++) {
int offY = (int)std::round((i / 4.0) * ampMaxPixel);
DrawLine(0, midY - offY, w, midY - offY, Color{32, 42, 54, 255});
DrawLine(0, midY + offY, w, midY + offY, Color{32, 42, 54, 255});
}
DrawLine(0, midY, w, midY, Color{60, 80, 100, 255});
{
MutexLock lock(snapMutex);
localSnapshot = latestSnapshot;
}
double baseFreq = frequenzaFondamentale.load();
int periodoAttuale = (int)std::round(SAMPLE_RATE / std::max(1.0, baseFreq));
int displaySamples = std::min(BUFFER_SAMPLES - periodoAttuale - 10, std::max(100, periodoAttuale * 2));
int startIdx = BUFFER_SAMPLES - displaySamples;
if (triggerAttivo.load() && isPlaying.load()) {
int searchStart = std::max(1, BUFFER_SAMPLES - displaySamples - periodoAttuale);
int searchEnd = std::min(BUFFER_SAMPLES - displaySamples, searchStart + periodoAttuale);
int peakIdx = searchStart;
double maxVal = -2.0;
for (int i = searchStart; i < searchEnd; i++) {
if (localSnapshot[i] > maxVal) { maxVal = localSnapshot[i]; peakIdx = i; }
}
int zeroIdx = peakIdx;
int minBack = std::max(1, peakIdx - periodoAttuale);
for (int j = peakIdx; j >= minBack; j--) {
if (localSnapshot[j - 1] < 0.0 && localSnapshot[j] >= 0.0) { zeroIdx = j; break; }
}
startIdx = zeroIdx;
}
VecInt cifre;
VecDouble freqs;
{
MutexLock lock(seqMutex);
cifre = cifreAttive;
freqs = frequenzeAttive;
}
if (mostraOndeSingole.load() && isPlaying.load() && !cifre.empty()) {
int numOsci = (int)cifre.size();
double scalaAmp = (0.65 / std::sqrt((double)numOsci)) * (altezzaOnda * 0.42);
int tipo = formaOnda.load();
double sfas = sfasamentoNorm.load();
for (int k = 0; k < numOsci; k++) {
Color c = calcolaColoreDaFrequenza(cifre[k] * baseFreq, 130);
double stepFase = (cifre[k] * baseFreq) / SAMPLE_RATE;
double distCentro = k - (numOsci - 1) / 2.0;
double faseOffset = -(distCentro * sfas);
Vector2 prevPt = {0, (float)midY};
for (int i = 0; i < displaySamples; i++) {
double f = (i * stepFase) + faseOffset;
f -= std::floor(f);
double val;
if (tipo == 1) val = 1.0 - 4.0 * std::abs(f - 0.5);
else if (tipo == 2) val = (f < 0.5) ? 1.0 : -1.0;
else if (tipo == 3) val = (2.0 * f) - 1.0;
else val = std::sin(2.0 * M_PI * f);
Vector2 pt = {(float)((i / (double)(displaySamples - 1)) * w), (float)(midY - val * scalaAmp)};
if (i > 0) DrawLineEx(prevPt, pt, 1.3f, c);
prevPt = pt;
}
}
}
Color coloreSomma = WHITE;
if (!freqs.empty()) {
double rTot = 0, gTot = 0, bTot = 0;
for (double f : freqs) {
Color col = calcolaColoreDaFrequenza(f);
rTot += col.r; gTot += col.g; bTot += col.b;
}
double normCol = std::sqrt((double)freqs.size());
coloreSomma = {
(unsigned char)std::min(255.0, std::round(rTot / normCol)),
(unsigned char)std::min(255.0, std::round(gTot / normCol)),
(unsigned char)std::min(255.0, std::round(bTot / normCol)),
255
};
}
Vector2 prevSum = {0, (float)(midY - localSnapshot[startIdx] * (altezzaOnda * 0.42))};
for (int i = 1; i < displaySamples; i++) {
Vector2 pt = {
(float)((i / (double)(displaySamples - 1)) * w),
(float)(midY - localSnapshot[startIdx + i] * (altezzaOnda * 0.42))
};
DrawLineEx(prevSum, pt, 3.0f, coloreSomma);
prevSum = pt;
}
// --- 2. SPETTRO PANORAMA STEREO ---
int topSpettro = topBarH + altezzaOnda;
DrawRectangle(0, topSpettro, w, altezzaSpettro, Color{20, 26, 34, 255});
DrawLineEx({0, (float)topSpettro}, {(float)w, (float)topSpettro}, 2.0f, Color{50, 65, 82, 255});
int margineX = 65;
int areaUtileX = w - (margineX * 2);
int baseY = topSpettro + altezzaSpettro - 35;
int maxBarHeight = altezzaSpettro - 70;
DrawLine(margineX - 20, baseY, w - margineX + 20, baseY, Color{55, 72, 90, 255});
for (int y = topSpettro + 24; y < baseY; y += 8) DrawLine(w / 2, y, w / 2, y + 4, Color{55, 72, 90, 255});
const char* titSpettro = "PANORAMA STEREO & OCCORRENZE ARMONICHE";
DrawText(titSpettro, (w - MeasureText(titSpettro, 12)) / 2, topSpettro + 8, 12, Color{140, 165, 190, 255});
DrawText("L (100% Left)", margineX - 35, baseY + 18, 12, Color{120, 150, 180, 255});
DrawText("C (Center)", (w / 2) - 30, baseY + 18, 12, Color{120, 150, 180, 255});
DrawText("R (100% Right)", w - margineX - 50, baseY + 18, 12, Color{120, 150, 180, 255});
if (!cifre.empty()) {
int occorrenze[10] = {0};
int maxOcc = 1;
for (int c : cifre) if (c >= 1 && c <= 9) maxOcc = std::max(maxOcc, ++occorrenze[c]);
int numOsci = (int)cifre.size();
int barWidth = std::max(10, std::min(38, (areaUtileX / std::max(1, numOsci)) - 10));
for (int k = 0; k < numOsci; k++) {
int c = cifre[k];
int count = occorrenze[c];
double normPan = (numOsci == 1) ? 0.5 : (k / (double)(numOsci - 1));
int centerX = margineX + (int)std::round(normPan * areaUtileX);
int barH = (int)std::round((count / (double)maxOcc) * maxBarHeight);
Rectangle barRect = {(float)(centerX - barWidth / 2), (float)(baseY - barH), (float)barWidth, (float)barH};
Color barCol = calcolaColoreDaFrequenza(freqs[k], 190);
Color borderCol = calcolaColoreDaFrequenza(freqs[k], 255);
DrawRectangleRounded(barRect, 0.2f, 6, barCol);
DrawRectangleRoundedLines(barRect, 0.2f, 6, 1.5f, borderCol);
char lblC[8]; std::snprintf(lblC, sizeof(lblC), "%d", c);
DrawText(lblC, centerX - MeasureText(lblC, 15) / 2, baseY + 3, 15, WHITE);
if (numOsci <= 22) {
char infoTop[32];
if (count > 1) std::snprintf(infoTop, sizeof(infoTop), "%.0fHz (x%d)", freqs[k], count);
else std::snprintf(infoTop, sizeof(infoTop), "%.0fHz", freqs[k]);
DrawText(infoTop, centerX - MeasureText(infoTop, 10) / 2, (int)barRect.y - 14, 10, Color{195, 215, 235, 255});
}
}
}
// --- 3. BARRA SUPERIORE ---
DrawRectangle(0, 0, w, topBarH, Color{26, 32, 40, 255});
DrawLine(0, topBarH, w, topBarH, Color{45, 55, 68, 255});
int curX = 20;
DrawText("Sequenza (1-9):", curX, 22, 15, Color{210, 225, 240, 255}); curX += 130;
Rectangle rectSeq = {(float)curX, 14, 240, 32};
DrawRectangleRec(rectSeq, activeTextBox == 1 ? Color{40, 50, 65, 255} : Color{18, 22, 28, 255});
DrawRectangleLinesEx(rectSeq, 1.5f, activeTextBox == 1 ? Color{100, 220, 170, 255} : Color{60, 75, 95, 255});
DrawText(txtSequenza.c_str(), curX + 8, 21, 18, WHITE);
if (CheckCollisionPointRec(GetMousePosition(), rectSeq) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) activeTextBox = 1;
curX += 255;
DrawText("Base (Hz):", curX, 22, 15, Color{210, 225, 240, 255}); curX += 85;
Rectangle rectBase = {(float)curX, 14, 90, 32};
DrawRectangleRec(rectBase, activeTextBox == 2 ? Color{40, 50, 65, 255} : Color{18, 22, 28, 255});
DrawRectangleLinesEx(rectBase, 1.5f, activeTextBox == 2 ? Color{100, 220, 170, 255} : Color{60, 75, 95, 255});
DrawText(txtFondamentale.c_str(), curX + 8, 21, 18, WHITE);
if (CheckCollisionPointRec(GetMousePosition(), rectBase) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) activeTextBox = 2;
curX += 105;
double valBase = std::atof(txtFondamentale.c_str());
std::string notaStr = calcolaNotaPitagorica(valBase);
Rectangle rectNota = {(float)curX, 14, 210, 32};
DrawRectangleLinesEx(rectNota, 1.0f, Color{55, 75, 90, 255});
DrawText(notaStr.c_str(), curX + 10, 22, 15, Color{100, 220, 170, 255});
curX += 225;
const char* playTxt = isPlaying.load() ? "Ferma / Aggiorna" : "Avvia Suono";
Color playCol = isPlaying.load() ? Color{160, 45, 45, 255} : Color{35, 130, 80, 255};
if (DrawButton({(float)curX, 14, 150, 32}, playTxt, playCol, Color{55, 160, 100, 255}, WHITE, 15)) {
if (isPlaying.load()) {
std::string pulita;
for (char c : txtSequenza) if (c >= '1' && c <= '9') pulita += c;
std::string spaced;
for (char c : pulita) { spaced += c; spaced += ' '; }
if (!spaced.empty()) spaced.pop_back();
std::string curSeq;
{ MutexLock l(seqMutex); curSeq = sequenzaCorrente; }
if (spaced != curSeq || std::abs(valBase - frequenzaFondamentale.load()) > 0.001) {
avviaOAggiorna(txtSequenza, txtFondamentale, msgErrore, msgErroreTimer);
} else {
isPlaying.store(false);
}
} else {
avviaOAggiorna(txtSequenza, txtFondamentale, msgErrore, msgErroreTimer);
}
}
// --- 4. BARRA INFERIORE ---
int botY = h - botBarH;
DrawRectangle(0, botY, w, botBarH, Color{26, 32, 40, 255});
DrawLine(0, botY, w, botY, Color{45, 55, 68, 255});
int bx = 20;
const char* singTxt = mostraOndeSingole.load() ? "Singole: ON" : "Singole: OFF";
if (DrawButton({(float)bx, (float)botY + 14, 115, 32}, singTxt, Color{40, 50, 65, 255}, Color{55, 70, 90, 255}, WHITE, 14)) {
mostraOndeSingole.store(!mostraOndeSingole.load());
}
bx += 125;
const char* trigTxt = triggerAttivo.load() ? "Trigger: ON" : "Trigger: OFF";
if (DrawButton({(float)bx, (float)botY + 14, 115, 32}, trigTxt, Color{40, 50, 65, 255}, Color{55, 70, 90, 255}, WHITE, 14)) {
triggerAttivo.store(!triggerAttivo.load());
}
bx += 130;
if (DrawButton({(float)bx, (float)botY + 14, 145, 32}, tipiOnda[formaOnda.load()], Color{45, 60, 80, 255}, Color{65, 85, 110, 255}, Color{100, 220, 170, 255}, 14)) {
formaOnda.store((formaOnda.load() + 1) % 4);
}
bx += 160;
char faseBuf[32]; std::snprintf(faseBuf, sizeof(faseBuf), "Fase: %d°", sliderFaseVal);
DrawText(faseBuf, bx, botY + 22, 14, Color{210, 225, 240, 255}); bx += 78;
Rectangle sldFaseRect = {(float)bx, (float)botY + 22, 120, 16};
DrawRectangleRec(sldFaseRect, Color{15, 20, 26, 255});
float knobFaseX = sldFaseRect.x + (sliderFaseVal / 360.0f) * sldFaseRect.width;
DrawCircle((int)knobFaseX, (int)(sldFaseRect.y + 8), 8, Color{100, 220, 170, 255});
if (CheckCollisionPointRec(GetMousePosition(), {sldFaseRect.x - 10, sldFaseRect.y - 8, sldFaseRect.width + 20, 32}) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
float norm = std::max(0.0f, std::min(1.0f, (GetMousePosition().x - sldFaseRect.x) / sldFaseRect.width));
sliderFaseVal = (int)std::round(norm * 360.0f);
sfasamentoNorm.store(sliderFaseVal / 360.0);
}
bx += 132;
if (DrawButton({(float)bx, (float)botY + 14, 75, 32}, "Sync 0°", Color{40, 50, 65, 255}, Color{55, 70, 90, 255}, WHITE, 13)) {
sliderFaseVal = 0;
sfasamentoNorm.store(0.0);
resetFasiRichiesto.store(true);
}
bx += 90;
char lpBuf[32]; std::snprintf(lpBuf, sizeof(lpBuf), "LP: %.0f Hz", cutoffLP.load());
DrawText(lpBuf, bx, botY + 22, 14, Color{210, 225, 240, 255}); bx += 92;
Rectangle sldLPRect = {(float)bx, (float)botY + 22, 140, 16};
DrawRectangleRec(sldLPRect, Color{15, 20, 26, 255});
float knobLPX = sldLPRect.x + (sliderLPVal / 1000.0f) * sldLPRect.width;
DrawCircle((int)knobLPX, (int)(sldLPRect.y + 8), 8, Color{255, 172, 20, 255});
if (CheckCollisionPointRec(GetMousePosition(), {sldLPRect.x - 10, sldLPRect.y - 8, sldLPRect.width + 20, 32}) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
float norm = std::max(0.0f, std::min(1.0f, (GetMousePosition().x - sldLPRect.x) / sldLPRect.width));
sliderLPVal = (int)std::round(norm * 1000.0f);
if (sliderLPVal == 0) cutoffLP.store(0.0);
else cutoffLP.store(10.0 * std::pow(16000.0 / 10.0, sliderLPVal / 1000.0));
}
if (msgErroreTimer > 0.0f) {
DrawRectangle(w / 2 - 260, topBarH + 15, 520, 36, Color{180, 40, 40, 230});
DrawText(msgErrore.c_str(), w / 2 - MeasureText(msgErrore.c_str(), 14) / 2, topBarH + 26, 14, WHITE);
}
EndDrawing();
}
isPlaying.store(false);
closeMidi();
ma_device_uninit(&audioDevice);
CloseWindow();
return 0;
}