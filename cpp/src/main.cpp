/*
 * NumSound - Harmonic Sequence Synthesizer & Stereo Panorama (v1.1)
 * Required Notice: Copyright (c) 2026 Federico Tubaro (https://github.com)
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "raylib.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOGDI
    #define NOUSER
    #define NOMINMAX
    #include <windows.h>
    #include <mmsystem.h>
    #pragma comment(lib, "winmm.lib")
#elif defined(__APPLE__)
    #include <CoreMIDI/CoreMIDI.h>
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
#include <iterator>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef std::vector<double> VecDouble;
typedef std::vector<int> VecInt;
typedef std::vector<char> VecChar;
typedef std::lock_guard<std::mutex> MutexLock;
typedef std::atomic<bool> AtomicBool;
typedef std::atomic<double> AtomicDouble;
typedef std::atomic<int> AtomicInt;

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

AtomicBool hold1Attivo{false};
AtomicBool hold2Attivo{false};

AtomicDouble attackSec{0.1};
AtomicDouble releaseSec{0.3};

AtomicInt temperamentoAttivo{0}; // 0=Pitagorico, 1=Equabile, 2=V. Galilei, 3=Mesotonico
AtomicInt notaRifAttiva{0};      // 0=DO3 (MIDI 60), 1=LA3 (MIDI 69)
AtomicDouble freqRifHz{256.0};

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

// --- POLIFONIA A 2 VOCI ---
struct Voice {
    bool gate = false;
    int midiNote = -1;
    double baseFreq = 64.0;
    double envelope = 0.0;
    double fasi[MAX_OSCI] = {0.0};
    unsigned long long triggerOrder = 0;
};

std::mutex voiceMutex;
std::vector<Voice> voices(2);
unsigned long long globalNoteCounter = 0;

// --- FUNZIONI MUSICALI E TEMPERAMENTI ---
const char* getNomeNota(int idx) {
    static const std::vector<const char*> NOTE = {
        "DO", "REb", "RE", "MIb", "MI", "FA", "FA#", "SOL", "LAb", "LA", "SIb", "SI"
    };
    return NOTE.at(idx % 12);
}

const char* getNomeTemperamento(int idx) {
    if (idx == 1) return "Equabile";
    if (idx == 2) return "V. Galilei";
    if (idx == 3) return "Mesotonico";
    return "Pitagorico";
}

const char* getNomeRif(int idx) {
    return (idx == 1) ? "LA3" : "DO3";
}

const char* getNomeOnda(int idx) {
    if (idx == 1) return "Triangolare";
    if (idx == 2) return "Onda Quadra";
    if (idx == 3) return "Dente di Sega";
    return "Sinusoide";
}

double getRapportoTemperamento(int tempIdx, int semitono) {
    static const VecDouble PITAGORICO = {
        1.0, 256.0/243.0, 9.0/8.0, 32.0/27.0, 81.0/64.0, 4.0/3.0,
        729.0/512.0, 3.0/2.0, 128.0/81.0, 27.0/16.0, 16.0/9.0, 243.0/128.0
    };
    static const VecDouble MESOTONICO = {
        1.0,
        8.0 * std::pow(5.0, -1.25),
        std::sqrt(5.0) / 2.0,
        4.0 * std::pow(5.0, -0.75),
        1.25,
        2.0 * std::pow(5.0, -0.25),
        std::pow(5.0, 1.5) / 8.0,
        std::pow(5.0, 0.25),
        1.6,
        std::pow(5.0, 0.75) / 2.0,
        4.0 / std::sqrt(5.0),
        std::pow(5.0, 1.25) / 4.0
    };
    if (tempIdx == 1) return std::pow(2.0, semitono / 12.0);
    if (tempIdx == 2) return std::pow(18.0 / 17.0, semitono);
    if (tempIdx == 3) return MESOTONICO.at(semitono);
    return PITAGORICO.at(semitono);
}

double getFreqDo3() {
    int temp = temperamentoAttivo.load();
    int rif = notaRifAttiva.load();
    double fRif = freqRifHz.load();
    if (rif == 0) return fRif;
    return fRif / getRapportoTemperamento(temp, 9);
}

std::string calcolaNotaTemperata(double freq) {
    if (freq <= 0.0) return "--";
    int temp = temperamentoAttivo.load();
    double refDo2 = getFreqDo3() / 2.0;
    if (refDo2 <= 0.0) return "--";

    int ottava = 2;
    double freqNorm = freq;
    while (freqNorm < refDo2) { freqNorm *= 2.0; ottava--; }
    while (freqNorm >= refDo2 * 2.0) { freqNorm /= 2.0; ottava++; }

    double rapportoCorrente = freqNorm / refDo2;
    int indiceMigliore = 0;
    double minDiffCents = 1e9;
    double freqNotaEsatta = refDo2;

    for (int i = 0; i < 12; i++) {
        double r = getRapportoTemperamento(temp, i);
        double diffCents = 1200.0 * std::abs(std::log(rapportoCorrente / r) / std::log(2.0));
        if (diffCents < minDiffCents) {
            minDiffCents = diffCents;
            indiceMigliore = i;
            freqNotaEsatta = r * refDo2 * std::pow(2.0, ottava - 2);
        }
    }
    double diffDoSup = 1200.0 * std::abs(std::log(rapportoCorrente / 2.0) / std::log(2.0));
    if (diffDoSup < minDiffCents) {
        minDiffCents = diffDoSup;
        indiceMigliore = 0;
        ottava++;
        freqNotaEsatta = refDo2 * std::pow(2.0, ottava - 2);
    }

    VecChar buf(64, 0);
    if (minDiffCents < 1.5) {
        std::snprintf(buf.data(), buf.size(), "%s%d (%.1f Hz)", getNomeNota(indiceMigliore), ottava, freqNotaEsatta);
    } else {
        std::snprintf(buf.data(), buf.size(), "~%s%d (%.1f Hz)", getNomeNota(indiceMigliore), ottava, freqNotaEsatta);
    }
    return std::string(buf.data());
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
        int cifra = testoPulito.at(i) - '0';
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

// --- CALLBACK AUDIO REAL-TIME (2 VOCI POLIFONICHE) ---
void audioCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pDevice; (void)pInput;
    float* out = (float*)pOutput;

    static double lpLeft1 = 0.0, lpLeft2 = 0.0;
    static double lpRight1 = 0.0, lpRight2 = 0.0;
    static VecDouble rollingHistory(BUFFER_SAMPLES, 0.0);
    static int contatoreGrafica = 0;
    static VecInt localCifre(MAX_OSCI, 0);
    static VecDouble gainLeft(MAX_OSCI, 0.0);
    static VecDouble gainRight(MAX_OSCI, 0.0);

    int numOsci = 0;
    {
        MutexLock lock(seqMutex);
        numOsci = std::min((int)cifreAttive.size(), MAX_OSCI);
        for (int i = 0; i < numOsci; i++) localCifre.at(i) = cifreAttive.at(i);
    }

    if (numOsci == 0) {
        isPlaying.store(false);
        std::memset(out, 0, frameCount * 2 * sizeof(float));
        return;
    }

    bool hold1 = hold1Attivo.load();
    bool hold2 = hold2Attivo.load();

    double manualBase = frequenzaFondamentale.load();

    std::vector<Voice> localVoices(2);
    {
        MutexLock lock(voiceMutex);
        if (resetFasiRichiesto.exchange(false)) {
            for (int v = 0; v < 2; v++) std::fill(std::begin(voices.at(v).fasi), std::end(voices.at(v).fasi), 0.0);
        }
        if (voices.at(0).midiNote == -1 && voices.at(1).midiNote == -1) {
            voices.at(0).baseFreq = manualBase;
        }
        localVoices = voices;
    }

    bool anyActive = false;
    for (int v = 0; v < 2; v++) {
        bool vHold = (v == 0) ? hold1 : (hold2 && (localVoices.at(1).midiNote != -1));
        bool vGate = localVoices.at(v).gate || vHold;
        if (vGate || localVoices.at(v).envelope > 0.0) anyActive = true;
    }

    if (!anyActive) {
        isPlaying.store(false);
        std::memset(out, 0, frameCount * 2 * sizeof(float));
        return;
    }
    isPlaying.store(true);

    for (int k = 0; k < numOsci; k++) {
        double angolo = (numOsci == 1) ? (M_PI / 4.0) : (k * (M_PI / 2.0) / (numOsci - 1));
        gainLeft.at(k) = std::cos(angolo);
        gainRight.at(k) = std::sin(angolo);
    }

    int tipoOnda = formaOnda.load();
    double cutoff = cutoffLP.load();
    double sfasamento = sfasamentoNorm.load();
    double attackStep = 1.0 / (SAMPLE_RATE * std::max(0.005, attackSec.load()));
    double releaseStep = 1.0 / (SAMPLE_RATE * std::max(0.005, releaseSec.load()));

    double alpha = 0.0;
    if (cutoff > 0.0) {
        double dt = 1.0 / SAMPLE_RATE;
        double rc = 1.0 / (2.0 * M_PI * cutoff);
        alpha = dt / (rc + dt);
    }

    if (frameCount < BUFFER_SAMPLES) {
        std::memmove(rollingHistory.data(), rollingHistory.data() + frameCount, (BUFFER_SAMPLES - frameCount) * sizeof(double));
    }

    double fattoreNorm = std::sqrt((double)numOsci * 1.5);

    for (ma_uint32 i = 0; i < frameCount; i++) {
        double sommaLeft = 0.0;
        double sommaRight = 0.0;

        for (int v = 0; v < 2; v++) {
            bool vHold = (v == 0) ? hold1 : (hold2 && (localVoices.at(1).midiNote != -1));
            bool vGate = localVoices.at(v).gate || vHold;
            if (vGate && localVoices.at(v).envelope < 1.0) {
                localVoices.at(v).envelope = std::min(1.0, localVoices.at(v).envelope + attackStep);
            } else if (!vGate && localVoices.at(v).envelope > 0.0) {
                localVoices.at(v).envelope = std::max(0.0, localVoices.at(v).envelope - releaseStep);
            }

            if (localVoices.at(v).envelope <= 0.0) continue;

            double vBase = localVoices.at(v).baseFreq;
            double vEnv = localVoices.at(v).envelope;

            for (int k = 0; k < numOsci; k++) {
                double fase = localVoices.at(v).fasi[k];
                double distCentro = k - (numOsci - 1) / 2.0;
                double faseEffettiva = fase - (distCentro * sfasamento);
                faseEffettiva -= std::floor(faseEffettiva);

                double onda;
                if (tipoOnda == 1) onda = 1.0 - 4.0 * std::abs(faseEffettiva - 0.5);
                else if (tipoOnda == 2) onda = (faseEffettiva < 0.5) ? 1.0 : -1.0;
                else if (tipoOnda == 3) onda = (2.0 * faseEffettiva) - 1.0;
                else onda = std::sin(2.0 * M_PI * faseEffettiva);

                sommaLeft += onda * gainLeft.at(k) * vEnv;
                sommaRight += onda * gainRight.at(k) * vEnv;

                fase += (localCifre.at(k) * vBase) / SAMPLE_RATE;
                if (fase >= 1.0) fase -= std::floor(fase);
                localVoices.at(v).fasi[k] = fase;
            }
        }

        lpLeft1 += alpha * (sommaLeft - lpLeft1);
        lpLeft2 += alpha * (lpLeft1 - lpLeft2);
        sommaLeft = lpLeft2;
        lpRight1 += alpha * (sommaRight - lpRight1);
        lpRight2 += alpha * (lpRight1 - lpRight2);
        sommaRight = lpRight2;

        double normLeft = std::tanh((sommaLeft / fattoreNorm) * 0.65) * 0.65;
        double normRight = std::tanh((sommaRight / fattoreNorm) * 0.65) * 0.65;
        *(out + i * 2) = (float)normLeft;
        *(out + i * 2 + 1) = (float)normRight;

        if (frameCount < BUFFER_SAMPLES) {
            rollingHistory.at((BUFFER_SAMPLES - frameCount) + i) = ((normLeft + normRight) * 0.5) * 2.0;
        }
    }

    {
        MutexLock lock(voiceMutex);
        for (int v = 0; v < 2; v++) {
            voices.at(v).envelope = localVoices.at(v).envelope;
            std::memcpy(voices.at(v).fasi, localVoices.at(v).fasi, sizeof(voices.at(v).fasi));
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
        double freq = getFreqDo3() * std::pow(2.0, ottave) * getRapportoTemperamento(temperamentoAttivo.load(), indiceNota);

        {
            MutexLock lock(voiceMutex);
            int targetVoice = -1;

            for (int v = 0; v < 2; v++) {
                if (voices.at(v).midiNote == notaMidi) { targetVoice = v; break; }
            }
            if (targetVoice == -1) {
                for (int v = 0; v < 2; v++) {
                    if (!voices.at(v).gate && voices.at(v).envelope < 0.01) { targetVoice = v; break; }
                }
            }
            if (targetVoice == -1) {
                for (int v = 0; v < 2; v++) {
                    if (!voices.at(v).gate) { targetVoice = v; break; }
                }
            }
            if (targetVoice == -1) {
                targetVoice = (voices.at(0).triggerOrder <= voices.at(1).triggerOrder) ? 0 : 1;
            }

            voices.at(targetVoice).midiNote = notaMidi;
            voices.at(targetVoice).baseFreq = freq;
            voices.at(targetVoice).gate = true;
            voices.at(targetVoice).triggerOrder = ++globalNoteCounter;
        }

        midiPendingFreq.store(freq);
        midiNotePending.store(true);
        isPlaying.store(true);
    } else if (cmd == 0x80 || (cmd == 0x90 && data2 == 0)) {
        MutexLock lock(voiceMutex);
        for (int v = 0; v < 2; v++) {
            if (voices[v].midiNote == data1) {
                voices[v].gate = false;
            }
        }
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

void parseMidiPacket(const MIDIPacket* packet) {
    const unsigned char* d = packet->data;
    UInt16 len = packet->length;
    UInt16 idx = 0;
    unsigned char runningStatus = 0;

    while (idx < len) {
        unsigned char byte = d[idx];
        if (byte >= 0xF8) { idx++; continue; }
        if (byte & 0x80) { runningStatus = byte; idx++; }
        if (runningStatus == 0) { idx++; continue; }

        unsigned char cmd = runningStatus & 0xF0;
        if (cmd == 0x80 || cmd == 0x90 || cmd == 0xA0 || cmd == 0xB0 || cmd == 0xE0) {
            if (idx + 1 < len) {
                processMidiBytes(runningStatus, d[idx], d[idx + 1]);
                idx += 2;
            } else break;
        } else if (cmd == 0xC0 || cmd == 0xD0) {
            if (idx < len) idx += 1;
            else break;
        } else {
            runningStatus = 0;
            idx++;
        }
    }
}

void initMidi() {
    OSStatus err = MIDIClientCreateWithBlock(CFSTR("NumSoundMIDI"), &midiClient, ^(const MIDINotification* message) {
        if (message->messageID == kMIDIMsgObjectAdded) {
            ItemCount numSources = MIDIGetNumberOfSources();
            for (ItemCount i = 0; i < numSources; ++i) {
                MIDIEndpointRef src = MIDIGetSource(i);
                if (src != 0) MIDIPortConnectSource(midiInPort, src, nullptr);
            }
        }
    });
    if (err != noErr) return;

    err = MIDIInputPortCreateWithBlock(midiClient, CFSTR("InputPort"), &midiInPort, ^(const MIDIPacketList* pktlist, void* srcConnRefCon) {
        (void)srcConnRefCon;
        const MIDIPacket* packet = pktlist->packet;
        for (UInt32 i = 0; i < pktlist->numPackets; ++i) {
            parseMidiPacket(packet);
            packet = MIDIPacketNext(packet);
        }
    });
    if (err != noErr) return;

    ItemCount numSources = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < numSources; ++i) {
        MIDIEndpointRef src = MIDIGetSource(i);
        if (src != 0) MIDIPortConnectSource(midiInPort, src, nullptr);
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

// --- HELPER UI ---
bool DrawButton(Rectangle rect, const char* text, Color bg, Color hoverBg, Color textColor, int fontSize = 15) {
    Vector2 mouse = GetMousePosition();
    bool hover = CheckCollisionPointRec(mouse, rect);
    DrawRectangleRounded(rect, 0.25f, 8, hover ? hoverBg : bg);
    DrawRectangleRoundedLines(rect, 0.25f, 8, 1.5f, Color{70, 90, 110, 255});
    int textW = MeasureText(text, fontSize);
    DrawText(text, (int)(rect.x + (rect.width - textW) / 2), (int)(rect.y + (rect.height - fontSize) / 2), fontSize, textColor);
    return hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

void avviaOAggiorna(const std::string& txtSequenza, const std::string& txtFondamentale, std::string& msgErrore, float& msgErroreTimer, bool attivaHold = true) {
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
    if (attivaHold) hold1Attivo.store(true);
    isPlaying.store(true);
}

// --- MAIN (PARTE INIZIALE E INPUT) ---
int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1340, 700, "NumSound — v. 1.1 - (c)2026 Federico Tubaro");
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
    std::string txtRifHz = "256";
    std::string txtAttack = "0.1";
    std::string txtRelease = "0.3";
    int activeTextBox = 1;
    int sliderLPVal = 1000;
    int sliderFaseVal = 0;
    std::string msgErrore = "";
    float msgErroreTimer = 0.0f;
    VecDouble localSnapshot(BUFFER_SAMPLES, 0.0);

    auto applicaBaseManuale = [&]() {
        double nuovaBase = std::atof(txtFondamentale.c_str());
        if (nuovaBase >= 1.0 && nuovaBase <= 4096.0) {
            frequenzaFondamentale.store(nuovaBase);
            MutexLock lock(voiceMutex);
            voices.at(0).baseFreq = nuovaBase;
            voices.at(0).midiNote = -1;
            voices.at(1).midiNote = -1;
            voices.at(0).gate = false;
            voices.at(1).gate = false;
        }
    };
    
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (msgErroreTimer > 0.0f) msgErroreTimer -= dt;

        if (midiNotePending.exchange(false)) {
            double freq = midiPendingFreq.load();
            VecChar buf(32, 0);
            std::snprintf(buf.data(), buf.size(), "%.2f", freq);
            txtFondamentale = buf.data();
            if (txtSequenza.empty()) txtSequenza = "1";
            std::string pulita;
            for (char c : txtSequenza) if (c >= '1' && c <= '9') pulita += c;
            if (!pulita.empty()) {
                aggiornaDatiSequenza(pulita, freq);
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
                applicaBaseManuale();
            } else if (activeTextBox == 3 && ((key >= '0' && key <= '9') || key == '.' || key == ',') && txtRifHz.size() < 8) {
                txtRifHz += (key == ',') ? '.' : (char)key;
                double rVal = std::atof(txtRifHz.c_str());
                if (rVal > 10.0 && rVal < 2000.0) freqRifHz.store(rVal);
            } else if (activeTextBox == 4 && ((key >= '0' && key <= '9') || key == '.' || key == ',') && txtAttack.size() < 5) {
                txtAttack += (key == ',') ? '.' : (char)key;
                attackSec.store(std::max(0.0, std::min(10.0, std::atof(txtAttack.c_str()))));
            } else if (activeTextBox == 5 && ((key >= '0' && key <= '9') || key == '.' || key == ',') && txtRelease.size() < 5) {
                txtRelease += (key == ',') ? '.' : (char)key;
                releaseSec.store(std::max(0.0, std::min(10.0, std::atof(txtRelease.c_str()))));
            }
            key = GetCharPressed();
        }

        if (IsKeyPressed(KEY_BACKSPACE)) {
            if (activeTextBox == 1 && !txtSequenza.empty()) {
                txtSequenza.pop_back();
            } else if (activeTextBox == 2 && !txtFondamentale.empty()) {
                txtFondamentale.pop_back();
                applicaBaseManuale();
            } else if (activeTextBox == 3 && !txtRifHz.empty()) {
                txtRifHz.pop_back();
                double rVal = std::atof(txtRifHz.c_str());
                if (rVal > 10.0 && rVal < 2000.0) freqRifHz.store(rVal);
            } else if (activeTextBox == 4 && !txtAttack.empty()) {
                txtAttack.pop_back();
                attackSec.store(std::max(0.0, std::min(10.0, std::atof(txtAttack.c_str()))));
            } else if (activeTextBox == 5 && !txtRelease.empty()) {
                txtRelease.pop_back();
                releaseSec.store(std::max(0.0, std::min(10.0, std::atof(txtRelease.c_str()))));
            }
        }

        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            avviaOAggiorna(txtSequenza, txtFondamentale, msgErrore, msgErroreTimer, true);
        }

        // Aggiornamento live di sequenza e frequenze anche in silenzio
        {
            std::string pulitaLive;
            for (char c : txtSequenza) {
                if (c >= '1' && c <= '9') pulitaLive += c;
            }
            double baseLive = std::atof(txtFondamentale.c_str());
            if (baseLive >= 1.0 && baseLive <= 4096.0) {
                std::string spacedLive;
                for (char c : pulitaLive) { spacedLive += c; spacedLive += ' '; }
                if (!spacedLive.empty()) spacedLive.pop_back();

                std::string curSeq;
                { MutexLock l(seqMutex); curSeq = sequenzaCorrente; }

                if (spacedLive != curSeq || std::abs(baseLive - frequenzaFondamentale.load()) > 0.0001) {
                    aggiornaDatiSequenza(pulitaLive, baseLive);
                    MutexLock lock(voiceMutex);
                    if (voices.at(0).midiNote == -1 && voices.at(1).midiNote == -1) {
                        voices.at(0).baseFreq = baseLive;
                    }
                }
            }
        }

        int w = GetScreenWidth();
        int h = GetScreenHeight();
        int topBarH = 88;
        int botBarH = 60;
        int centerH = h - topBarH - botBarH;
        int altezzaSpettro = std::max(215, (int)(centerH * 0.40f));
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


        VecInt cifre;
        VecDouble freqs;
        {
            MutexLock lock(seqMutex);
            cifre = cifreAttive;
            freqs = frequenzeAttive;
        }
        
        //Legge frequenza ed envelope di entrambe le voci e disegna le onde singole di entrambe:
        
        double vBase[2] = {baseFreq, baseFreq};
        double vEnv[2] = {0.0, 0.0};
        bool vUsed[2] = {true, false};
        {
            MutexLock lock(voiceMutex);
            for (int v = 0; v < 2; v++) {
                vBase[v] = voices.at(v).baseFreq;
                vEnv[v] = voices.at(v).envelope;
                vUsed[v] = (v == 0) || (voices.at(v).midiNote != -1) || (voices.at(v).envelope > 0.001);
            }
        }

        // Usa la frequenza della Voce 1 se attiva, altrimenti Voce 2, per calcolare il periodo del trigger
        double triggerFreq = (*(vEnv + 0) > 0.001) ? *(vBase + 0) : ((*(vEnv + 1) > 0.001) ? *(vBase + 1) : baseFreq);
        periodoAttuale = (int)std::round(SAMPLE_RATE / std::max(1.0, triggerFreq));
        displaySamples = std::min(BUFFER_SAMPLES - periodoAttuale * 2 - 10, std::max(100, periodoAttuale * 2));
        startIdx = BUFFER_SAMPLES - displaySamples;

        if (triggerAttivo.load() && isPlaying.load()) {
            // Cerca il primo attraversamento dello zero in salita con isteresi (Schmitt Trigger)
            // partendo da una finestra fissa prima della fine del buffer
            int searchEnd = BUFFER_SAMPLES - displaySamples;
            int searchStart = std::max(1, searchEnd - periodoAttuale * 2);

            // Calcola il picco nella finestra per adattare la soglia di isteresi al volume attuale
            double peakLocal = 0.0;
            for (int i = searchStart; i < searchEnd; i++) {
                peakLocal = std::max(peakLocal, std::abs(localSnapshot.at(i)));
            }

            double armThreshold = -peakLocal * 0.25; // Deve scendere sotto -25% prima di armare il trigger
            bool armed = false;
            int foundIdx = -1;

            for (int i = searchStart; i < searchEnd; i++) {
                double sPrev = localSnapshot.at(i - 1);
                double sCurr = localSnapshot.at(i);
                if (sCurr < armThreshold) {
                    armed = true;
                } else if (armed && sPrev < 0.0 && sCurr >= 0.0) {
                    foundIdx = i;
                    break; // Prende sempre il PRIMO attraversamento valido, senza saltare tra i picchi!
                }
            }

            if (foundIdx != -1) {
                // Interpolazione sub-campione per eliminare anche il micro-tremolio di 1 pixel
                double s0 = localSnapshot.at(foundIdx - 1);
                double s1 = localSnapshot.at(foundIdx);
                double frac = (s1 - s0 > 1e-9) ? (-s0 / (s1 - s0)) : 0.0;
                startIdx = foundIdx;
                // Nota: frac può essere usato se vuoi ancora più precisione, ma già foundIdx elimina il salto di ciclo
                (void)frac;
            }
        }

        if (mostraOndeSingole.load() && !cifre.empty()) {
            int numOsci = (int)cifre.size();
            bool playingNow = isPlaying.load();

            // Determina quali voci mostrare:
            // - Se suona: mostra le voci attive (env > 0)
            // - Se in silenzio: mostra le ultime voci usate (Voce 1 sempre, Voce 2 se ha suonato almeno una volta)
            std::vector<bool> showVoice = {true, false};
            if (playingNow) {
                showVoice.at(0) = (*(vEnv + 0) > 0.001);
                showVoice.at(1) = (*(vEnv + 1) > 0.001);
                if (!showVoice.at(0) && !showVoice.at(1)) showVoice.at(0) = true;
            } else {
                showVoice.at(0) = true;
                showVoice.at(1) = *(vUsed + 1);
            }

            int numVociVis = (showVoice.at(0) ? 1 : 0) + (showVoice.at(1) ? 1 : 0);

            double scalaAmp = (0.65 / std::sqrt((double)numOsci * (numVociVis > 1 ? 1.5 : 1.0))) * (altezzaOnda * 0.42);
            int tipo = formaOnda.load();
            double sfas = sfasamentoNorm.load();

            for (int v = 0; v < 2; v++) {
                if (!showVoice.at(v)) continue;

                double envVal = *(vEnv + v);
                double fFond = *(vBase + v);

                // In silenzio: traccia fantasma sottile e sbiadita (0.7px, alpha 45)
                // Durante il suono: si accende e si ispessisce seguendo l'inviluppo
                float spessoreOnda = playingNow ? (0.7f + (float)(0.8 * envVal)) : 0.7f;
                unsigned char alphaOnda = playingNow ? (unsigned char)std::round(45.0 + 95.0 * envVal) : 45;

                for (int k = 0; k < numOsci; k++) {
                    Color c = calcolaColoreDaFrequenza(cifre.at(k) * fFond, alphaOnda);
                    double stepFase = (cifre.at(k) * fFond) / SAMPLE_RATE;
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
                        if (i > 0) DrawLineEx(prevPt, pt, spessoreOnda, c);
                        prevPt = pt;
                    }
                }
            }
        }
		//Fine lettura e disegno onde singole
		

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

        if (isPlaying.load()) {
            Vector2 prevSum = {0, (float)(midY - localSnapshot.at(startIdx) * (altezzaOnda * 0.42))};
            for (int i = 1; i < displaySamples; i++) {
                Vector2 pt = {
                    (float)((i / (double)(displaySamples - 1)) * w),
                    (float)(midY - localSnapshot.at(startIdx + i) * (altezzaOnda * 0.42))
                };
                DrawLineEx(prevSum, pt, 6.0f, coloreSomma);
                prevSum = pt;
            }
        }

        // --- 2. SPETTRO PANORAMA STEREO SPECULARE (VOCE 1 SU / VOCE 2 GIU) ---
        int topSpettro = topBarH + altezzaOnda;
        DrawRectangle(0, topSpettro, w, altezzaSpettro, Color{20, 26, 34, 255});
        DrawLineEx({0, (float)topSpettro}, {(float)w, (float)topSpettro}, 2.0f, Color{50, 65, 82, 255});

        int margineX = 75;
        int areaUtileX = w - (margineX * 2);
		int zeroY = topSpettro + (altezzaSpettro - 18) / 2;
		int maxBarHalfH = ((altezzaSpettro - 18) / 2) - 34;

        // Linea mediana dello Zero e linea tratteggiata del Centro Stereo
        DrawLine(margineX - 25, zeroY, w - margineX + 25, zeroY, Color{85, 108, 132, 255});
        for (int y = topSpettro + 22; y < topSpettro + altezzaSpettro - 18; y += 8) {
            DrawLine(w / 2, y, w / 2, y + 4, Color{55, 72, 90, 255});
        }

        const char* titSpettro = "PANORAMA STEREO & OCCORRENZE ARMONICHE (VOCE 1 / VOCE 2)";
        DrawText(titSpettro, (w - MeasureText(titSpettro, 11)) / 2, topSpettro + 6, 11, Color{140, 165, 190, 255});

        // Etichette VOCE 1 (sopra la linea zero) e VOCE 2 (sotto la linea zero)
        DrawText("VOCE 1", 12, zeroY - 18, 11, Color{100, 220, 170, 255});
        DrawText("VOCE 2", 12, zeroY + 6, 11, Color{130, 200, 255, 255});

        DrawText("L (100% Left)", margineX - 35, topSpettro + altezzaSpettro - 15, 11, Color{120, 150, 180, 255});
        DrawText("C (Center)", (w / 2) - 28, topSpettro + altezzaSpettro - 15, 11, Color{120, 150, 180, 255});
        DrawText("R (100% Right)", w - margineX - 45, topSpettro + altezzaSpettro - 15, 11, Color{120, 150, 180, 255});

        if (!cifre.empty()) {
            VecInt occorrenze(10, 0);
            int maxOcc = 1;
            for (int c : cifre) if (c >= 1 && c <= 9) maxOcc = std::max(maxOcc, ++occorrenze.at(c));
            int numOsci = (int)cifre.size();
            int barWidth = std::max(10, std::min(38, (areaUtileX / std::max(1, numOsci)) - 10));

            for (int k = 0; k < numOsci; k++) {
                int c = cifre.at(k);
                int count = occorrenze.at(c);
                double normPan = (numOsci == 1) ? 0.5 : (k / (double)(numOsci - 1));
                int centerX = margineX + (int)std::round(normPan * areaUtileX);
                double ratioOcc = count / (double)maxOcc;

                // --- VOCE 1 (cresce verso l'ALTO da zeroY) ---
                double freq1 = c * vBase[0];
                int fullH1 = (int)std::round(ratioOcc * maxBarHalfH);
                int barH1 = (int)std::round(fullH1 * vEnv[0]);

                Color col1 = calcolaColoreDaFrequenza(freq1, 200);

                Color bord1 = calcolaColoreDaFrequenza(freq1, 255);
                // Sagoma massima tratteggiata/sottile di riferimento
                DrawRectangleLines(centerX - barWidth / 2, zeroY - fullH1, barWidth, fullH1, Color{bord1.r, bord1.g, bord1.b, 33});
                if (barH1 > 0) {
                    Rectangle r1 = {(float)(centerX - barWidth / 2), (float)(zeroY - barH1), (float)barWidth, (float)barH1};
                    DrawRectangleRounded(r1, 0.2f, 6, col1);
                    DrawRectangleRoundedLines(r1, 0.2f, 6, 1.5f, bord1);
                }

                // --- VOCE 2 (cresce verso il BASSO da zeroY) ---
                if (vUsed[1]) {
                    double freq2 = c * vBase[1];
                    int fullH2 = (int)std::round(ratioOcc * maxBarHalfH);
                    int barH2 = (int)std::round(fullH2 * vEnv[1]);

                    Color col2 = calcolaColoreDaFrequenza(freq2, 200);

                    Color bord2 = calcolaColoreDaFrequenza(freq2, 255);
                    DrawRectangleLines(centerX - barWidth / 2, zeroY + 1, barWidth, fullH2, Color{bord2.r, bord2.g, bord2.b, 55});
                    if (barH2 > 0) {
                        Rectangle r2 = {(float)(centerX - barWidth / 2), (float)(zeroY + 1), (float)barWidth, (float)barH2};
                        DrawRectangleRounded(r2, 0.2f, 6, col2);
                        DrawRectangleRoundedLines(r2, 0.2f, 6, 1.5f, bord2);
                    }
                    if (numOsci <= 22 && fullH2 > 4) {
                        VecChar infoBot(32, 0);
                        std::snprintf(infoBot.data(), infoBot.size(), "%.0fHz", freq2);
                        DrawText(infoBot.data(), centerX - MeasureText(infoBot.data(), 9) / 2, zeroY + fullH2 + 3, 9, Color{165, 195, 225, 255});
                    }
                }

                // Cifra sulla linea dello zero + Frequenza Voce 1 in cima
                VecChar lblC(8, 0);
                std::snprintf(lblC.data(), lblC.size(), "%d", c);
                int cW = MeasureText(lblC.data(), 13);
                DrawRectangle(centerX - cW / 2 - 2, zeroY - 7, cW + 4, 14, Color{20, 26, 34, 220});
                DrawText(lblC.data(), centerX - cW / 2, zeroY - 6, 13, WHITE);

                if (numOsci <= 22) {
                    VecChar infoTop(32, 0);
                    if (count > 1) std::snprintf(infoTop.data(), infoTop.size(), "%.0fHz (x%d)", freq1, count);
                    else std::snprintf(infoTop.data(), infoTop.size(), "%.0fHz", freq1);
                    DrawText(infoTop.data(), centerX - MeasureText(infoTop.data(), 9) / 2, zeroY - fullH1 - 12, 9, Color{195, 215, 235, 255});
                }
            }
        }

        // --- 3. BARRA SUPERIORE ---
        DrawRectangle(0, 0, w, topBarH, Color{26, 32, 40, 255});
        DrawLine(0, topBarH, w, topBarH, Color{45, 55, 68, 255});

        Color colLabelTop = Color{155, 180, 205, 255};
        Color colGroupBg = Color{21, 26, 33, 255};
        Color colGroupBorder = Color{52, 66, 84, 255};

        int labelY = 14;
        int boxY = 38;
        int boxH = 34;
        int curX = 16;

        // 1. SEQUENZA ARMONICI
        DrawText("SEQUENZA ARMONICI", curX, labelY, 11, colLabelTop);
        Rectangle rectSeq = {(float)curX, (float)boxY, 175, (float)boxH};
        DrawRectangleRec(rectSeq, activeTextBox == 1 ? Color{40, 50, 65, 255} : Color{18, 22, 28, 255});
        DrawRectangleLinesEx(rectSeq, 1.5f, activeTextBox == 1 ? Color{100, 220, 170, 255} : Color{60, 75, 95, 255});
        DrawText(txtSequenza.c_str(), curX + 8, boxY + 8, 18, WHITE);
        if (CheckCollisionPointRec(GetMousePosition(), rectSeq) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) activeTextBox = 1;
        curX += 188;

        // 2. FREQ. (Hz) CIFRA 1
        DrawText("FREQ. (Hz) CIFRA 1", curX, labelY, 11, Color{100, 220, 170, 255});
        Rectangle rectBase = {(float)curX, (float)boxY, 110, (float)boxH};
        DrawRectangleRec(rectBase, activeTextBox == 2 ? Color{40, 50, 65, 255} : Color{18, 22, 28, 255});
        DrawRectangleLinesEx(rectBase, 1.5f, activeTextBox == 2 ? Color{100, 220, 170, 255} : Color{60, 75, 95, 255});
        DrawText(txtFondamentale.c_str(), curX + 8, boxY + 8, 17, WHITE);
        if (CheckCollisionPointRec(GetMousePosition(), rectBase) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) activeTextBox = 2;
        curX += 122;

        // 3. GRUPPO: NOTE riferite a TEMPERAMENTO (V1 sopra / V2 sotto)
        int grpNotaW = 286;
        Rectangle rectGrpNota = {(float)curX - 6, 8, (float)grpNotaW, (float)topBarH - 16};
        DrawRectangleRounded(rectGrpNota, 0.12f, 6, colGroupBg);
        DrawRectangleRoundedLines(rectGrpNota, 0.12f, 6, 1.0f, colGroupBorder);

        DrawText("NOTE riferite a TEMPERAMENTO", curX + 2, labelY, 11, colLabelTop);

        double valBase = std::atof(txtFondamentale.c_str());
        std::string notaStr1 = "V1: " + calcolaNotaTemperata(*(vBase + 0) > 0.0 ? *(vBase + 0) : valBase);
        std::string notaStr2 = *(vUsed + 1) ? ("V2: " + calcolaNotaTemperata(*(vBase + 1))) : "V2: --";

        Rectangle rectNota = {(float)curX, (float)boxY, 160, (float)boxH};
        DrawRectangleRec(rectNota, Color{16, 21, 27, 255});
        DrawRectangleLinesEx(rectNota, 1.0f, Color{55, 75, 90, 255});
        DrawText(notaStr1.c_str(), curX + 6, boxY + 4, 11, Color{100, 220, 170, 255});
        DrawText(notaStr2.c_str(), curX + 6, boxY + 19, 11, Color{130, 200, 255, 255});

        if (DrawButton({(float)curX + 166, (float)boxY, 108, (float)boxH}, getNomeTemperamento(temperamentoAttivo.load()), Color{45, 60, 80, 255}, Color{65, 85, 110, 255}, Color{255, 210, 100, 255}, 14)) {
            temperamentoAttivo.store((temperamentoAttivo.load() + 1) % 4);
        }
        curX += grpNotaW + 10;

        // 4. GRUPPO: DIAPASON
        int grpDiapW = 162;
        Rectangle rectGrpDiap = {(float)curX - 6, 8, (float)grpDiapW, (float)topBarH - 16};
        DrawRectangleRounded(rectGrpDiap, 0.12f, 6, colGroupBg);
        DrawRectangleRoundedLines(rectGrpDiap, 0.12f, 6, 1.0f, colGroupBorder);

        const char* titDiap = "DIAPASON";
        int titDiapW = MeasureText(titDiap, 11);
        DrawText(titDiap, (int)(rectGrpDiap.x + (grpDiapW - titDiapW) / 2), 11, 11, Color{130, 200, 255, 255});

        DrawText("NOME NOTA", curX, 24, 9, colLabelTop);
        DrawText("FREQ.(Hz)", curX + 72, 24, 9, colLabelTop);

        if (DrawButton({(float)curX, (float)boxY, 62, (float)boxH}, getNomeRif(notaRifAttiva.load()), Color{45, 60, 80, 255}, Color{65, 85, 110, 255}, Color{130, 200, 255, 255}, 14)) {
            int nuovoRif = (notaRifAttiva.load() + 1) % 2;
            notaRifAttiva.store(nuovoRif);
            if (nuovoRif == 1 && txtRifHz == "256") {
                txtRifHz = "432";
                freqRifHz.store(432.0);
            } else if (nuovoRif == 0 && (txtRifHz == "432" || txtRifHz == "440")) {
                txtRifHz = "256";
                freqRifHz.store(256.0);
            }
        }

        Rectangle rectRif = {(float)curX + 70, (float)boxY, 80, (float)boxH};
        DrawRectangleRec(rectRif, activeTextBox == 3 ? Color{40, 50, 65, 255} : Color{18, 22, 28, 255});
        DrawRectangleLinesEx(rectRif, 1.5f, activeTextBox == 3 ? Color{100, 220, 170, 255} : Color{60, 75, 95, 255});
        DrawText(txtRifHz.c_str(), curX + 76, boxY + 9, 16, WHITE);
        if (CheckCollisionPointRec(GetMousePosition(), rectRif) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) activeTextBox = 3;

        curX += grpDiapW + 10;

        // 5. PULSANTI HOLD 1 & HOLD 2
        bool isHold1 = hold1Attivo.load();
        bool isHold2 = hold2Attivo.load();
        Color h1Col = isHold1 ? Color{35, 130, 80, 255} : Color{55, 68, 85, 255};
        Color h2Col = isHold2 ? Color{35, 110, 155, 255} : Color{55, 68, 85, 255};

        DrawText("HOLD VOCI", curX + 26, labelY, 11, colLabelTop);
        if (DrawButton({(float)curX, (float)boxY, 54, (float)boxH}, isHold1 ? "H1:ON" : "H1:OFF", h1Col, Color{45, 155, 95, 255}, WHITE, 12)) {
            if (!isHold1) {
                avviaOAggiorna(txtSequenza, txtFondamentale, msgErrore, msgErroreTimer, false);
                if (msgErroreTimer <= 0.0f) hold1Attivo.store(true);
            } else {
                hold1Attivo.store(false);
            }
        }
        if (DrawButton({(float)curX + 58, (float)boxY, 54, (float)boxH}, isHold2 ? "H2:ON" : "H2:OFF", h2Col, Color{50, 135, 185, 255}, WHITE, 12)) {
            if (!isHold2) {
                avviaOAggiorna(txtSequenza, txtFondamentale, msgErrore, msgErroreTimer, false);
                if (msgErroreTimer <= 0.0f) hold2Attivo.store(true);
            } else {
                hold2Attivo.store(false);
            }
        }
        curX += 122;

        // 6. GRUPPO INVILUPPO: ATTACCO (0-10s) & RILASCIO (0-10s)
        int grpEnvW = 294;
        Rectangle rectGrpEnv = {(float)curX - 6, 8, (float)grpEnvW, (float)topBarH - 16};
        DrawRectangleRounded(rectGrpEnv, 0.12f, 6, colGroupBg);
        DrawRectangleRoundedLines(rectGrpEnv, 0.12f, 6, 1.0f, colGroupBorder);

        // ATTACCO
        DrawText("ATTACCO", curX + 2, 12, 11, colLabelTop);
        DrawText("(0-10s)", curX + 10, 24, 9, Color{130, 155, 180, 255});

        Rectangle sldAttRect = {(float)curX, (float)boxY + 10, 80, 14};
        DrawRectangleRec(sldAttRect, Color{15, 20, 26, 255});
        float attNorm = (float)(attackSec.load() / 10.0);
        DrawCircle((int)(sldAttRect.x + attNorm * sldAttRect.width), (int)(sldAttRect.y + 7), 7, Color{100, 220, 170, 255});
        if (CheckCollisionPointRec(GetMousePosition(), {sldAttRect.x - 6, sldAttRect.y - 8, sldAttRect.width + 12, 30}) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            float norm = std::max(0.0f, std::min(1.0f, (GetMousePosition().x - sldAttRect.x) / sldAttRect.width));
            double newVal = std::round(norm * 100.0) / 10.0;
            attackSec.store(newVal);
            VecChar b(16, 0);
            std::snprintf(b.data(), b.size(), "%.1f", newVal);
            txtAttack = b.data();
        }

        Rectangle rectAttBox = {(float)curX + 86, (float)boxY, 46, (float)boxH};
        DrawRectangleRec(rectAttBox, activeTextBox == 4 ? Color{40, 50, 65, 255} : Color{18, 22, 28, 255});
        DrawRectangleLinesEx(rectAttBox, 1.5f, activeTextBox == 4 ? Color{100, 220, 170, 255} : Color{60, 75, 95, 255});
        DrawText(txtAttack.c_str(), curX + 92, boxY + 9, 15, WHITE);
        if (CheckCollisionPointRec(GetMousePosition(), rectAttBox) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) activeTextBox = 4;

        curX += 146;

        // RILASCIO
        DrawText("RILASCIO", curX + 2, 12, 11, colLabelTop);
        DrawText("(0-10s)", curX + 10, 24, 9, Color{130, 155, 180, 255});

        Rectangle sldRelRect = {(float)curX, (float)boxY + 10, 80, 14};
        DrawRectangleRec(sldRelRect, Color{15, 20, 26, 255});
        float relNorm = (float)(releaseSec.load() / 10.0);
        DrawCircle((int)(sldRelRect.x + relNorm * sldRelRect.width), (int)(sldRelRect.y + 7), 7, Color{100, 220, 170, 255});
        if (CheckCollisionPointRec(GetMousePosition(), {sldRelRect.x - 6, sldRelRect.y - 8, sldRelRect.width + 12, 30}) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            float norm = std::max(0.0f, std::min(1.0f, (GetMousePosition().x - sldRelRect.x) / sldRelRect.width));
            double newVal = std::round(norm * 100.0) / 10.0;
            releaseSec.store(newVal);
            VecChar b(16, 0);
            std::snprintf(b.data(), b.size(), "%.1f", newVal);
            txtRelease = b.data();
        }

        Rectangle rectRelBox = {(float)curX + 86, (float)boxY, 46, (float)boxH};
        DrawRectangleRec(rectRelBox, activeTextBox == 5 ? Color{40, 50, 65, 255} : Color{18, 22, 28, 255});
        DrawRectangleLinesEx(rectRelBox, 1.5f, activeTextBox == 5 ? Color{100, 220, 170, 255} : Color{60, 75, 95, 255});
        DrawText(txtRelease.c_str(), curX + 92, boxY + 9, 15, WHITE);
        if (CheckCollisionPointRec(GetMousePosition(), rectRelBox) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) activeTextBox = 5;

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

        if (DrawButton({(float)bx, (float)botY + 14, 145, 32}, getNomeOnda(formaOnda.load()), Color{45, 60, 80, 255}, Color{65, 85, 110, 255}, Color{100, 220, 170, 255}, 14)) {
            formaOnda.store((formaOnda.load() + 1) % 4);
        }
        bx += 160;

        VecChar faseBuf(32, 0);
        std::snprintf(faseBuf.data(), faseBuf.size(), "Fase: %d°", sliderFaseVal);
        DrawText(faseBuf.data(), bx, botY + 22, 14, Color{210, 225, 240, 255});
        bx += 78;
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

        VecChar lpBuf(32, 0);
        std::snprintf(lpBuf.data(), lpBuf.size(), "LP: %.0f Hz", cutoffLP.load());
        DrawText(lpBuf.data(), bx, botY + 22, 14, Color{210, 225, 240, 255});
        bx += 92;

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
