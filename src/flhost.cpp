#include "flhost.h"
#include <stdarg.h>
#include <share.h>
#include <algorithm>

static FILE *g_log = nullptr;

void HostLog(const char *fmt, ...) {
    if (!g_log) {
        char path[MAX_PATH];
        GetTempPathA(MAX_PATH, path);
        strcat_s(path, "wasp-bridge.log");
        g_log = _fsopen(path, "a", _SH_DENYNO);   // readable while the host runs
        if (!g_log) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static LRESULT CALLBACK AppWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static HWND MakeAppWindow() {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = AppWndProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"WaspBridgeApp";
        RegisterClassW(&wc);
        registered = true;
    }
    return CreateWindowExW(0, L"WaspBridgeApp", L"Wasp Bridge", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
}

FLHost::FLHost() {
    HostVersion = 11001001;   // pretend to be FL 11.1.1, what these plugins were built against
    Flags = 0;
    AppHandle = nullptr;
    memset(WaveTables, 0, sizeof(WaveTables));
    memset(TempBuffers, 0, sizeof(TempBuffers));
    memset(Reserved, 0, sizeof(Reserved));
    BuildWaveTables();
    AllocBuffers();
}

FLHost::~FLHost() { Unload(); }

void FLHost::BuildWaveTables() {
    const int N = WaveT_Size;
    const double PI = 3.14159265358979323846;
    for (auto &t : waveTables) t.resize(N);
    uint32_t seed = 12345;
    for (int i = 0; i < N; i++) {
        double p = (double)i / N;
        waveTables[0][i] = (float)sin(2 * PI * p);                                   // sine
        waveTables[1][i] = (float)(p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4); // triangle
        waveTables[2][i] = p < 0.5 ? 1.f : -1.f;                                     // square
        waveTables[3][i] = (float)(2 * p - 1);                                       // saw
        // "analog" saw: band-limited-ish saw (sum of harmonics)
        double s = 0;
        for (int k = 1; k <= 64; k++) s += sin(2 * PI * k * p) / k;
        waveTables[4][i] = (float)(-s * 2 / PI);
        seed = seed * 1664525u + 1013904223u;
        waveTables[5][i] = (float)((int32_t)seed / 2147483648.0);                    // noise
    }
    for (int i = 0; i < 6; i++) WaveTables[i] = (PWaveT)waveTables[i].data();
}

void FLHost::AllocBuffers() {
    tempBuf.assign((size_t)blockSize * 2 * 4 + 64, 0.f);
    for (int i = 0; i < 4; i++) TempBuffers[i] = (PWAV32FS)(tempBuf.data() + (size_t)i * blockSize * 2);
    voiceBuf.assign((size_t)blockSize * 2 + 64, 0.f);
}

bool FLHost::Load(const wchar_t *dllPath) {
    Unload();
    pluginDir = dllPath;
    size_t slash = pluginDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) pluginDir.resize(slash + 1);
    char tmp[MAX_PATH * 2];
    WideCharToMultiByte(CP_ACP, 0, pluginDir.c_str(), -1, tmp, sizeof(tmp), nullptr, nullptr);
    pluginDirA = tmp;

    if (!AppHandle) AppHandle = MakeAppWindow();

    dll = LoadLibraryExW(dllPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { HostLog("LoadLibrary failed %lu", GetLastError()); return false; }
    auto create = (TCreatePlugInstance)GetProcAddress(dll, "CreatePlugInstance");
    if (!create) { HostLog("no CreatePlugInstance"); return false; }
    plug = create(this, 1);
    if (!plug || !plug->Info) { HostLog("CreatePlugInstance returned null"); plug = nullptr; return false; }
    plug->HostTag = 1;
    HostLog("loaded %s flags=0x%x params=%d poly=%d sdk=%d", plug->Info->LongName, plug->Info->Flags,
            plug->Info->NumParams, plug->Info->DefPoly, plug->Info->SDKVersion);

    plug->Dispatcher(FPD_SetSampleRate, 0, (intptr_t)sampleRate);
    plug->Dispatcher(FPD_SetBlockSize, 0, blockSize);
    plug->Dispatcher(FPD_SetNumSends, 0, 0);
    plug->Dispatcher(FPD_ProcessMode, 0, PM_Normal);
    SetTempo(tempo);
    plug->Dispatcher(FPD_SetEnabled, 0, 1);
    return true;
}

void FLHost::Unload() {
    if (plug) {
        std::lock_guard<std::recursive_mutex> g(mix);
        for (auto &v : voices) plug->Voice_Kill(v.handle);
        voices.clear();
        plug->Dispatcher(FPD_SetEnabled, 0, 0);
        plug->DestroyObject();
        plug = nullptr;
    }
    if (dll) { FreeLibrary(dll); dll = nullptr; }
}

void FLHost::SetSampleRate(double sr) {
    sampleRate = sr;
    if (plug) { plug->Dispatcher(FPD_SetSampleRate, 0, (intptr_t)sr); SetTempo(tempo); }
}

void FLHost::SetBlockSize(int n) {
    if (n < 64) n = 64;
    std::lock_guard<std::recursive_mutex> g(mix);
    blockSize = n;
    AllocBuffers();
    if (plug) plug->Dispatcher(FPD_SetBlockSize, 0, n);
}

static const int PPQ = 96;

void FLHost::SetTempo(double bpm) {
    if (bpm <= 0) return;
    tempo = bpm;
    if (!plug) return;
    float t = (float)bpm, spt = (float)(sampleRate * 60.0 / (bpm * PPQ));
    plug->ProcessEvent(FPE_Tempo, *(int *)&t, (int)spt);
    plug->Dispatcher(FPD_SetSamplesPerTick, 0, *(int *)&spt);
}

void FLHost::SetPlaying(bool playing) {
    if (plug) plug->Dispatcher(FPD_SetPlaying, 0, playing ? 1 : 0);
}

FLHost::Voice *FLHost::FindVoice(intptr_t tag) {
    for (auto &v : voices) if (v.tag == tag) return &v;
    return nullptr;
}

// FL voice pitch is in cents relative to C5 (MIDI 60); WB_PITCH_OFFSET overrides, for testing
static int PitchOffset() {
    static int off = [] { char b[32]; return GetEnvironmentVariableA("WB_PITCH_OFFSET", b, 32) ? atoi(b) : 6000; }();
    return off;
}

void FLHost::NoteOn(int note, float velocity) {
    if (!plug) return;
    std::lock_guard<std::recursive_mutex> g(mix);
    Voice v = {};
    v.tag = nextTag++;
    v.note = note;
    v.velocity = velocity;
    TLevelParams lp = {};
    lp.Pan = 0;
    lp.Vol = velocity;
    lp.Pitch = (float)(note * 100 - PitchOffset()) + pitchBend;
    v.params.InitLevels = lp;
    v.params.FinalLevels = lp;
    voices.push_back(v);
    Voice &nv = voices.back();
    SyncOldParams(nv);
    // the plugin keeps this pointer for the voice's life, so `voices` must not reallocate
    nv.handle = plug->TriggerVoice(OldParams() ? (PVoiceParams)&nv.oldParams : &nv.params, nv.tag);
    if (nv.handle == FVH_Null) voices.pop_back();
    // safety: drop released voices the plugin never reported as finished
    // (FL Studio itself does not do this; the test host skips it too when WB_TEST_NOTES is set)
    static const size_t limit = GetEnvironmentVariableA("WB_TEST_NOTES", nullptr, 0) ? 100000 : 64;
    while (voices.size() > limit) { Voice &old = voices.front(); TVoiceHandle h = old.handle; voices.pop_front(); plug->Voice_Kill(h); }
}

void FLHost::NoteOff(int note) {
    if (!plug) return;
    std::lock_guard<std::recursive_mutex> g(mix);
    for (auto &v : voices)
        if (v.note == note && !v.released) { v.released = true; plug->Voice_Release(v.handle); }
}

void FLHost::AllNotesOff() {
    if (!plug) return;
    std::lock_guard<std::recursive_mutex> g(mix);
    for (auto &v : voices)
        if (!v.released) { v.released = true; plug->Voice_Release(v.handle); }
}

void FLHost::PitchBend(float cents) {
    std::lock_guard<std::recursive_mutex> g(mix);
    pitchBend = cents;
    for (auto &v : voices) {
        v.params.FinalLevels.Pitch = (float)(v.note * 100 - PitchOffset()) + cents;
        SyncOldParams(v);
    }
    if (plug) plug->ProcessEvent(FPE_MIDI_Pitch, (int)cents, 0);
}

void FLHost::SyncOldParams(Voice &v) {
    auto conv = [](const TLevelParams &n, TLevelParams_Old &o) {
        o.Pan = (int)(n.Pan * 64); o.Vol = n.Vol; o.Pitch = (int)n.Pitch; o.FCut = n.FCut; o.FRes = n.FRes;
    };
    conv(v.params.InitLevels, v.oldParams.InitLevels);
    conv(v.params.FinalLevels, v.oldParams.FinalLevels);
}

void FLHost::RemoveVoice(intptr_t tag, bool killInPlugin) {
    for (auto it = voices.begin(); it != voices.end(); ++it) {
        if (it->tag == tag) {
            TVoiceHandle h = it->handle;
            voices.erase(it);
            if (killInPlugin && plug) {
                // never re-enter the plugin while it is rendering; kill right after
                if (inRender) pendingKill.push_back(h);
                else plug->Voice_Kill(h);
            }
            return;
        }
    }
}

void FLHost::Render(float *out, int frames) {
    memset(out, 0, sizeof(float) * 2 * frames);
    if (!plug) return;
    std::lock_guard<std::recursive_mutex> g(mix);
    const bool hybrid = (plug->Info->Flags & FPF_UseSampler) != 0;
    const bool wantTick = (plug->Info->Flags & FPF_WantNewTick) != 0;
    const double spt = sampleRate * 60.0 / (tempo * PPQ);
    int done = 0;
    while (done < frames) {
        int n = std::min(frames - done, blockSize);
        if (wantTick) {
            // split on tick boundaries so the plugin's NewTick runs at the right time
            double toNext = (floor(mixTicks) + 1 - mixTicks) * spt;
            int untilTick = (int)ceil(toNext);
            if (untilTick < 1) untilTick = 1;
            if (untilTick < n) n = untilTick;
        }
        float *dst = out + (size_t)done * 2;
        if (hybrid) {
            for (auto it = voices.begin(); it != voices.end();) {
                Voice &v = *it++;
                memset(voiceBuf.data(), 0, sizeof(float) * 2 * n);
                int len = n;
                int r = plug->Voice_Render(v.handle, (PWAV32FS)voiceBuf.data(), len);
                float L, R;
                ComputeLRVol(L, R, v.params.FinalLevels.Pan, v.params.FinalLevels.Vol);
                if (!v.lastL && !v.lastR) { v.lastL = L; v.lastR = R; }
                AddWave_32FS_32FS_Ramp(voiceBuf.data(), dst, len, L, R, v.lastL, v.lastR);
                if (r == FVR_NoMoreData) RemoveVoice(v.tag, true);
            }
        } else {
            int len = n;
            inRender = true;
            plug->Gen_Render((PWAV32FS)dst, len);
            inRender = false;
            for (TVoiceHandle h : pendingKill) plug->Voice_Kill(h);
            pendingKill.clear();
        }
        double before = mixTicks;
        mixTicks += n / spt;
        if (wantTick && floor(mixTicks) != floor(before)) plug->NewTick();
        done += n;
    }
}

// ---------------- TFruityPlugHost ----------------

intptr_t _stdcall FLHost::Dispatcher(TPluginTag Sender, intptr_t ID, intptr_t Index, intptr_t Value) {
    static char buf[MAX_PATH * 2];
    switch (ID) {
    case FHD_GetInstance:
        return (intptr_t)GetModuleHandleW(nullptr);
    case FHD_GetProgPath: {
        // FL root = the folder above "\Plugins\"
        std::string p = pluginDirA;
        size_t k = p.find("\\Plugins\\");
        if (k != std::string::npos) p.resize(k + 1);
        strcpy_s(buf, p.c_str());
        return (intptr_t)buf;
    }
    case FHD_LocateDataFile: {
        const char *name = (const char *)Value;
        if (!name) return 0;
        if (GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES) { strcpy_s(buf, name); return (intptr_t)buf; }
        std::string p = pluginDirA + name;
        strcpy_s(buf, p.c_str());
        return (intptr_t)buf;
    }
    case FHD_MsgBox: {
        const char *s = (const char *)Index;
        std::string t = s ? s : "", title = "Wasp", msg = t;
        size_t bar = t.find('|');
        if (bar != std::string::npos) { title = t.substr(0, bar); msg = t.substr(bar + 1); }
        return MessageBoxA((HWND)AppHandle, msg.c_str(), title.c_str(), (UINT)Value);
    }
    case FHD_GetMixingTime:
    case FHD_GetPlaybackTime:
        if (Value) {
            PFPTime t = (PFPTime)Value;
            t->t = Index == 0 ? mixTicks / PPQ : mixTicks * 60000.0 / (tempo * PPQ);
            t->t2 = 0;
        }
        return 1;
    case FHD_GetTimeMul:
        return 0;
    case FHD_GetParamMenuEntry:
        return 0;
    default:
        HostLog("host dispatcher id=%d index=%d value=%d", (int)ID, (int)Index, (int)Value);
        return 0;
    }
}

void _stdcall FLHost::OnParamChanged(TPluginTag, int Index, int Value) {}
void _stdcall FLHost::OnHint(TPluginTag, char *Text) {}

void _stdcall FLHost::ComputeLRVol_Old(float &LVol, float &RVol, int Pan, float Volume) {
    ComputeLRVol(LVol, RVol, Pan / 64.f, Volume);
}

void _stdcall FLHost::ComputeLRVol(float &LVol, float &RVol, float Pan, float Volume) {
    if (Pan < -1) Pan = -1;
    if (Pan > 1) Pan = 1;
    LVol = Volume * (Pan > 0 ? 1 - Pan : 1);
    RVol = Volume * (Pan < 0 ? 1 + Pan : 1);
}

void _stdcall FLHost::Voice_Release(intptr_t Sender) {
    std::lock_guard<std::recursive_mutex> g(mix);
    if (Voice *v = FindVoice(Sender)) if (!v->released) { v->released = true; plug->Voice_Release(v->handle); }
}

void _stdcall FLHost::Voice_Kill(intptr_t Sender, BOOL KillHandle) {
    // plugin says the voice has finished (normally after its release)
    std::lock_guard<std::recursive_mutex> g(mix);
    RemoveVoice(Sender, KillHandle != 0);
}

int _stdcall FLHost::Voice_ProcessEvent(intptr_t Sender, intptr_t EventID, intptr_t EventValue, intptr_t Flags) {
    Voice *v = FindVoice(Sender);
    float f;
    switch (EventID) {
    case FPV_GetLength: return -1;
    case FPV_GetColor: return 0;
    case FPV_GetVelocity: f = v ? v->velocity : 0.8f; return *(int *)&f;
    case FPV_GetRelVelocity: f = 0.5f; return *(int *)&f;
    case FPV_GetRelTime: f = 1.0f; return *(int *)&f;
    default: return 0;
    }
}

void _stdcall FLHost::LockMix() { mix.lock(); }
void _stdcall FLHost::UnlockMix() { mix.unlock(); }
void _stdcall FLHost::MIDIOut_Delayed(TPluginTag, intptr_t) {}
void _stdcall FLHost::MIDIOut(TPluginTag, intptr_t) {}

void _stdcall FLHost::AddWave_32FM_32FS_Ramp(void *SourceBuffer, void *DestBuffer, int Length, float LVol, float RVol, float &LastLVol, float &LastRVol) {
    float *s = (float *)SourceBuffer, *d = (float *)DestBuffer;
    float dl = Length ? (LVol - LastLVol) / Length : 0, dr = Length ? (RVol - LastRVol) / Length : 0;
    for (int i = 0; i < Length; i++) {
        LastLVol += dl; LastRVol += dr;
        d[2 * i] += s[i] * LastLVol;
        d[2 * i + 1] += s[i] * LastRVol;
    }
    LastLVol = LVol; LastRVol = RVol;
}

void _stdcall FLHost::AddWave_32FS_32FS_Ramp(void *SourceBuffer, void *DestBuffer, int Length, float LVol, float RVol, float &LastLVol, float &LastRVol) {
    float *s = (float *)SourceBuffer, *d = (float *)DestBuffer;
    float dl = Length ? (LVol - LastLVol) / Length : 0, dr = Length ? (RVol - LastRVol) / Length : 0;
    for (int i = 0; i < Length; i++) {
        LastLVol += dl; LastRVol += dr;
        d[2 * i] += s[2 * i] * LastLVol;
        d[2 * i + 1] += s[2 * i + 1] * LastRVol;
    }
    LastLVol = LVol; LastRVol = RVol;
}

bool _stdcall FLHost::LoadSample(TSampleHandle &Handle, char *, PWaveFormatExtensible, int) { Handle = 0; return false; }
void *_stdcall FLHost::GetSampleData(TSampleHandle, int &Length) { Length = 0; return nullptr; }
void _stdcall FLHost::CloseSample(TSampleHandle) {}
int _stdcall FLHost::GetSongMixingTime() { return (int)mixTicks; }
double _stdcall FLHost::GetSongMixingTime_A() { return mixTicks; }
double _stdcall FLHost::GetSongPlayingTime() { return mixTicks; }
void _stdcall FLHost::OnControllerChanged(TPluginTag, intptr_t, intptr_t) {}
void *_stdcall FLHost::GetSendBuffer(intptr_t) { return TempBuffers[3]; }
void _stdcall FLHost::PlugMsg_Delayed(TPluginTag, intptr_t Msg) { if (plug) plug->MsgIn(Msg); }
void _stdcall FLHost::PlugMsg_Kill(TPluginTag, intptr_t) {}
void _stdcall FLHost::GetSampleInfo(TSampleHandle, PSampleInfo Info) {}

void _stdcall FLHost::DistWave_32FM(int DistType, int DistThres, void *SourceBuffer, int Length, float DryVol, float WetVol, float Mul) {
    // TS404-style distortion: amplify then fold/clip at a threshold
    float *s = (float *)SourceBuffer;
    float thr = 1.f / (DistThres < 1 ? 1 : DistThres);
    for (int i = 0; i < Length; i++) {
        float x = s[i] * Mul, w;
        if (DistType == 0) w = x > thr ? thr : x < -thr ? -thr : x;
        else { w = x; while (w > thr || w < -thr) w = w > thr ? 2 * thr - w : -2 * thr - w; }
        s[i] = s[i] * DryVol + w / thr * WetVol;
    }
}

void *_stdcall FLHost::GetMixBuffer(int) { return TempBuffers[3]; }
void *_stdcall FLHost::GetInsBuffer(TPluginTag, int) { return TempBuffers[3]; }
bool _stdcall FLHost::PromptEdit(int, int, char *, char *, int &) { return false; }
void _stdcall FLHost::SuspendOutput() { mix.lock(); }
void _stdcall FLHost::ResumeOutput() { mix.unlock(); }
void _stdcall FLHost::GetSampleRegion(TSampleHandle, int, PSampleRegion) {}
void _stdcall FLHost::LockPlugin(TPluginTag) { mix.lock(); }
void _stdcall FLHost::UnlockPlugin(TPluginTag) { mix.unlock(); }
void _stdcall FLHost::LockMix_Shared_Old() { mix.lock(); }
void _stdcall FLHost::UnlockMix_Shared_Old() { mix.unlock(); }
void _stdcall FLHost::GetInBuffer(TPluginTag, intptr_t, PIOBuffer IBuffer) { if (IBuffer) { IBuffer->Buffer = nullptr; IBuffer->Flags = 0; } }
void _stdcall FLHost::GetOutBuffer(TPluginTag, intptr_t, PIOBuffer OBuffer) { if (OBuffer) { OBuffer->Buffer = nullptr; OBuffer->Flags = 0; } }
TOutVoiceHandle _stdcall FLHost::TriggerOutputVoice(TVoiceParams *, intptr_t, intptr_t) { return FVH_Null; }
void _stdcall FLHost::OutputVoice_Release(TOutVoiceHandle) {}
void _stdcall FLHost::OutputVoice_Kill(TOutVoiceHandle) {}
int _stdcall FLHost::OutputVoice_ProcessEvent(TOutVoiceHandle, intptr_t, intptr_t, intptr_t) { return 0; }
