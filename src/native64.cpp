// Wasp_x64.dll / Wasp XT_x64.dll replacement: a real 64-bit FL native plugin that
// forwards every call to WaspHost32.exe, which runs Image-Line's original 32-bit
// Wasp. Because FL loads it under the original name, old projects open with
// their Wasp channels and saved patches intact.
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <map>
#include "../sdk/fp_plugclass.h"
#include "ipc.h"

static HMODULE g_self;

static void Log(const char *fmt, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    char path[MAX_PATH]; GetTempPathA(MAX_PATH, path); strcat_s(path, "wasp-bridge64.log");
    FILE *f = _fsopen(path, "a", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static std::wstring Dir(const std::wstring &p) { return p.substr(0, p.find_last_of(L"\\/") + 1); }
static bool Exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

class Bridge64 : public TFruityPlug {
public:
    TFruityPlugHost *host;
    TFruityPlugInfo info = {};
    char longName[128] = "WASP", shortName[128] = "WASP";
    WBShared *S = nullptr;
    HANDLE map = nullptr, evAudioReq = nullptr, evAudioResp = nullptr;
    HWND helperWnd = nullptr;
    HWND hostWnd = nullptr;   // receives WM_WB_HOST from the helper, on FL's GUI thread
    HANDLE helper = nullptr;
    std::atomic<bool> broken{true};
    // self-healing (see Tick/Restart): what a fresh helper needs to continue where the old one stopped
    std::wstring dllPath, exePath;
    volatile bool pendingRestart = false;
    DWORD lostAt = 0, restartWindow = 0, lastSnapshot = 0;
    int restarts = 0;
    std::mutex stateLock;
    std::vector<uint8_t> lastState;                      // the patch: FL's last load/save, refreshed every 2 s
    std::map<std::pair<int, int>, intptr_t> settings;    // (FPD id, index) -> value of the host settings FL sent
    std::map<int, std::pair<int, int>> events;           // ProcessEvent id -> (value, flags): tempo, poly, pan...
    TTimeSigInfo timeSig = {}; bool haveTimeSig = false;
    intptr_t editorParent = 0;                           // FPD_ShowEditor's parent while the editor is open
    std::recursive_mutex audioLock, voiceLock;
    volatile DWORD guiThread;
    // voice slots: FL's params pointer + FL's tag for each active voice
    PVoiceParams flParams[WB_MAXV] = {};
    intptr_t flTag[WB_MAXV] = {};
    bool used[WB_MAXV] = {};
    uint32_t serial[WB_MAXV] = {}, nextSerial = 0;   // trigger order, to steal the oldest when all slots are taken

    Bridge64(TFruityPlugHost *h, TPluginTag tag) : host(h) {
        HostTag = tag;
        Info = &info;
        guiThread = GetCurrentThreadId();
        info.SDKVersion = 1;
        info.LongName = longName;
        info.ShortName = shortName;
        info.Flags = 0x400011;
        info.NumParams = 0;
        Locate();
        CreateHostWindow();
        Start();
    }

    ~Bridge64() {
        if (S && !broken) Call(true, OP_DESTROY);
        Stop(3000);
        if (hostWnd) { KillTimer(hostWnd, 1); DestroyWindow(hostWnd); }
    }

    // helper -> FL: runs a host dispatcher call the plugin needs answered synchronously
    static LRESULT CALLBACK HostWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Bridge64 *self = (Bridge64 *)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_TIMER) { if (self) self->Tick(); return 0; }
        if (m != WM_WB_HOST && m != WM_WB_PARAM && m != WM_WB_CTRL) return DefWindowProcW(h, m, w, l);
        if (!self || !self->S || self->broken) return 0;
        // knob moves arrive the moment they happen (not on the next Idle), so live automation recording keeps its timing
        if (m == WM_WB_PARAM) { self->host->OnParamChanged(self->HostTag, (int)w, (int)l); return 1; }
        if (m == WM_WB_CTRL) { self->host->OnControllerChanged(self->HostTag, (intptr_t)w, (intptr_t)(int)l); return 1; }
        if (w >= WB_GUIFRAMES) return 0;
        WBHostCall *c = &self->S->host[w];
        MemoryBarrier();
        c->entryName[sizeof(c->entryName) - 1] = 0;
        if (c->id == WB_HOST_ONHINT) { self->host->OnHint(self->HostTag, c->entryName); c->result = 0; return 1; }
        intptr_t value = c->textIn ? (intptr_t)c->entryName : (intptr_t)c->value;
        intptr_t r = self->host->Dispatcher(self->HostTag, (intptr_t)c->id, (intptr_t)c->index, value);
        c->hasEntry = 0;
        if (c->id == FHD_GetParamMenuEntry) {
            PParamMenuEntry e = (PParamMenuEntry)r;
            if (e) {
                strncpy_s(c->entryName, e->Name ? e->Name : "", _TRUNCATE);
                c->entryFlags = e->Flags;
                c->hasEntry = 1;
            }
            r = 0;   // pointers don't cross; the helper builds its own entry
        }
        c->result = r;
        MemoryBarrier();
        return 1;
    }

    void CreateHostWindow() {
        static ATOM cls = 0;
        if (!cls) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = HostWndProc;
            wc.hInstance = g_self;
            wc.lpszClassName = L"WaspBridgeHost";
            cls = RegisterClassW(&wc);
        }
        hostWnd = CreateWindowExW(0, L"WaspBridgeHost", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_self, nullptr);
        if (hostWnd) { SetWindowLongPtrW(hostWnd, GWLP_USERDATA, (LONG_PTR)this); SetTimer(hostWnd, 1, 1000, nullptr); }
    }

    // ---- self-healing --------------------------------------------------------------------------
    // If the helper dies or stops answering, Call() marks the bridge broken and the timer on hostWnd
    // (FL's GUI thread, so it also works with the editor closed) starts a new helper and brings it back
    // to where the old one was: the host settings FL had sent, the channel events, the last snapshot
    // of the patch, and the editor. Voices FL still thinks are playing are reported dead.
    void Lost() {
        broken = true;
        lostAt = GetTickCount();
        pendingRestart = true;
    }

    void Stop(DWORD wait) {
        broken = true;
        if (helper) { if (WaitForSingleObject(helper, wait) != WAIT_OBJECT_0) TerminateProcess(helper, 0); CloseHandle(helper); helper = nullptr; }
        for (HANDLE *x : { &evAudioReq, &evAudioResp }) if (*x) { CloseHandle(*x); *x = nullptr; }
        if (S) { UnmapViewOfFile(S); S = nullptr; }
        if (map) { CloseHandle(map); map = nullptr; }
        helperWnd = nullptr;
    }

    void Remember(intptr_t ID, intptr_t Index, intptr_t Value) {
        std::lock_guard<std::mutex> g(stateLock);
        switch (ID) {
        case FPD_ShowEditor: editorParent = Value; break;
        case FPD_SetTimeSig: if (Value) { timeSig = *(TTimeSigInfo *)Value; haveTimeSig = true; } break;
        case FPD_ProcessMode: case FPD_SetBlockSize: case FPD_SetSampleRate: case FPD_UseVoiceLevels: case FPD_SetEnabled:
        case FPD_SetPlaying: case FPD_SetInternalParam: case FPD_SetNumSends: case FPD_SetFitTime: case FPD_SetSamplesPerTick:
        case FPD_SetIdleTime: case FPD_SetIOLatency: case FPD_PreferredNumIO:
            settings[{ (int)ID, (int)Index }] = Value; break;
        }
    }

    void Tick() {
        if (pendingRestart) { Restart(); return; }
        if (broken) return;
        DWORD now = GetTickCount();
        if (now - lastSnapshot < 2000) return;
        lastSnapshot = now;
        std::vector<uint8_t> data;
        Call(true, OP_SAVE, 0, 0, 0, 0, nullptr, 0, &data);
        if (!data.empty() && !broken) { std::lock_guard<std::mutex> g(stateLock); lastState.swap(data); }
    }

    void Restart() {
        DWORD now = GetTickCount();
        if (now - lostAt < 1000) return;                                   // let the failure settle first
        std::unique_lock<std::recursive_mutex> a(audioLock, std::try_to_lock);
        if (!a.owns_lock()) return;                                        // a render still waits on the old helper: next tick
        pendingRestart = false;
        if (now - restartWindow > 60000) { restartWindow = now; restarts = 0; }
        if (++restarts > 3) { Log("the helper keeps failing: giving up (reload the channel to try again)"); return; }
        Log("restarting the helper (%d/3)", restarts);
        Stop(0);
        std::vector<intptr_t> dead;
        {
            std::lock_guard<std::recursive_mutex> g(voiceLock);
            for (int i = 0; i < WB_MAXV; i++) if (used[i]) { dead.push_back(flTag[i]); used[i] = false; flParams[i] = nullptr; }
        }
        for (intptr_t t : dead) host->Voice_Kill(t, TRUE);
        Start();
        if (broken) { lostAt = GetTickCount(); pendingRestart = true; return; }   // counted; tried again in a second
        std::map<std::pair<int, int>, intptr_t> st; std::map<int, std::pair<int, int>> ev; std::vector<uint8_t> state;
        TTimeSigInfo ts; bool hts; intptr_t parent;
        {
            std::lock_guard<std::mutex> g(stateLock);
            st = settings; ev = events; state = lastState; ts = timeSig; hts = haveTimeSig; parent = editorParent;
        }
        auto late = [](int id) { return id == FPD_SetEnabled || id == FPD_SetPlaying; };
        for (auto &s : st) if (!late(s.first.first)) Call(true, OP_DISPATCHER, s.first.first, s.first.second, s.second);
        if (hts) Call(true, OP_DISPATCHER, FPD_SetTimeSig, 0, 0, 0, &ts, sizeof(ts));
        for (auto &e : ev) Call(true, OP_PROCESSEVENT, e.first, e.second.first, e.second.second);
        if (!state.empty()) Call(true, OP_LOAD, 0, 0, 0, 0, state.data(), (int)state.size());
        for (auto &s : st) if (late(s.first.first)) Call(true, OP_DISPATCHER, s.first.first, s.first.second, s.second);
        DrainQueue();
        if (parent) Call(true, OP_DISPATCHER, FPD_ShowEditor, 0, parent);
        Log(broken ? "restart failed" : "helper restarted, %d state bytes restored", (int)state.size());
    }

    void Locate() {
        wchar_t me[MAX_PATH]; GetModuleFileNameW(g_self, me, MAX_PATH);
        std::wstring dir = Dir(me), file = std::wstring(me).substr(dir.size());
        std::wstring name = file.substr(0, file.rfind(L"_x64"));   // "Wasp" or "Wasp XT"
        if (name.empty() || name == file) name = file.substr(0, file.find_last_of(L'.'));
        bool xt = name.find(L"XT") != std::wstring::npos;
        strcpy_s(longName, xt ? "WASP XT" : "WASP"); strcpy_s(shortName, longName);
        info.NumParams = xt ? 56 : 41;

        // the original 32-bit plugin & our helper: x86\ next to us, else an older FL install
        std::wstring dll, exe;
        std::vector<std::wstring> cands = { dir + L"x86\\" + name + L".dll", dir + name + L".dll" };
        // not FL Studio 20: its Wasp build crashes in the helper
        for (const wchar_t *v : { L"FL Studio 12", L"FL Studio 11", L"FL Studio 8" })
            cands.push_back(std::wstring(L"C:\\Program Files\\Image-Line\\") + v + L"\\Plugins\\Fruity\\Generators\\" + name + L"\\" + name + L".dll");
        for (auto &c : cands) if (Exists(c)) { dll = c; break; }
        for (auto &c : { dir + L"x86\\WaspHost32.exe", dir + L"WaspHost32.exe" }) if (Exists(c)) { exe = c; break; }
        if (dll.empty() || exe.empty()) Log("missing: dll='%ls' helper='%ls'", dll.c_str(), exe.c_str());
        dllPath = dll; exePath = exe;
    }

    void Start() {
        const std::wstring &dll = dllPath, &exe = exePath;
        if (dll.empty() || exe.empty()) return;
        static LONG counter = 0;
        wchar_t id[64]; swprintf_s(id, L"%lu.%ld.%llu", GetCurrentProcessId(), InterlockedIncrement(&counter), (unsigned long long)GetTickCount64());
        auto nm = [&](const wchar_t *s) { return std::wstring(L"Local\\WaspBridge.") + id + s; };
        map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(WBShared), nm(L".mem").c_str());
        if (!map) { Log("CreateFileMapping failed %lu", GetLastError()); return; }
        S = (WBShared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WBShared));
        memset(S, 0, sizeof(WBShared));
        S->magic = WB_MAGIC;
        S->hostHwnd = (int64_t)(intptr_t)hostWnd;
        evAudioReq = CreateEventW(nullptr, FALSE, FALSE, nm(L".audioReq").c_str());
        evAudioResp = CreateEventW(nullptr, FALSE, FALSE, nm(L".audioResp").c_str());
        HANDLE evReady = CreateEventW(nullptr, TRUE, FALSE, nm(L".ready").c_str());

        std::wstring cmd = L"\"" + exe + L"\" " + id + L" \"" + dll + L"\" " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, Dir(dll).c_str(), &si, &pi)) {
            Log("CreateProcess failed %lu", GetLastError()); CloseHandle(evReady); return;
        }
        CloseHandle(pi.hThread);
        helper = pi.hProcess;
        // while it loads, Wasp may already call FL (e.g. FHD_GetMainFormHandle), so serve sent messages
        HANDLE w[2] = { evReady, helper };
        DWORD r, until = GetTickCount() + 20000;
        for (;;) {
            DWORD left = (DWORD)std::max<LONG>(0, (LONG)(until - GetTickCount()));
            r = MsgWaitForMultipleObjects(2, w, FALSE, left, QS_SENDMESSAGE);
            if (r != WAIT_OBJECT_0 + 2) break;
            MSG m; PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        CloseHandle(evReady);
        if (r != WAIT_OBJECT_0 || !S->ready) { Log("helper failed to start (wait=%lu error=%d)", r, S->error); return; }
        info.SDKVersion = S->sdk; info.Flags = S->flags; info.NumParams = S->numParams; info.DefPoly = S->defPoly;
        info.NumOutCtrls = S->numOutCtrls; info.NumOutVoices = S->numOutVoices;
        strcpy_s(longName, S->longName); strcpy_s(shortName, S->shortName);
        helperWnd = (HWND)(intptr_t)S->helperHwnd;
        broken = false;
        Log("started %ls via %ls, flags=0x%x params=%d", dll.c_str(), exe.c_str(), info.Flags, info.NumParams);
    }

    bool OnGuiThread() const { return GetCurrentThreadId() == guiThread; }

    // Sends one request and waits for the helper. Returns the result; copies response
    // data into out (if given), then replays callbacks the plugin raised meanwhile.
    // GUI requests go by SendMessage so nested calls in either direction behave like
    // in-process ones; audio requests use the event pair (no window traffic on the mixer thread).
    int64_t Call(bool gui, int op, int64_t a = 0, int64_t b = 0, int64_t c = 0, int64_t d = 0,
                 const void *in = nullptr, int inLen = 0, std::vector<uint8_t> *out = nullptr) {
        if (broken) return 0;
        std::vector<WBCallback> cbs;
        int64_t result;
        auto fill = [&](WBChannel *ch) {
            ch->op = op; ch->a = a; ch->b = b; ch->c = c; ch->d = d;
            ch->dataLen = 0; ch->nCb = 0; ch->result = 0;
            if (in && inLen > 0) { int n = std::min(inLen, WB_DATA); memcpy(ch->data, in, n); ch->dataLen = n; }
        };
        auto collect = [&](WBChannel *ch) {
            MemoryBarrier();
            result = ch->result;
            if (out) out->assign(ch->data, ch->data + std::max(0, std::min(ch->dataLen, WB_DATA)));
            cbs.assign(ch->cb, ch->cb + std::max(0, std::min(ch->nCb, WB_MAXCB)));
        };
        if (gui) {
            int f = -1;
            for (int i = 0; i < WB_GUIFRAMES && f < 0; i++)
                if (InterlockedCompareExchange((volatile LONG *)&S->guiBusy[i], 1, 0) == 0) f = i;
            if (f < 0) { Log("GUI requests nested too deep (op %d)", op); return 0; }
            WBChannel *ch = &S->gui[f];
            fill(ch);
            MemoryBarrier();
            DWORD_PTR res = 0;
            if (!SendMessageTimeoutW(helperWnd, WM_WB_CALL, f, 0, SMTO_NORMAL, 10000, &res)) {
                DWORD err = GetLastError();
                bool dead = WaitForSingleObject(helper, 0) == WAIT_OBJECT_0;
                Log("GUI request op %d failed (%s, err %lu)", op, dead ? "helper died" : "timeout", err);
                S->guiBusy[f] = 0;
                Lost();
                return 0;
            }
            collect(ch);
            MemoryBarrier();
            S->guiBusy[f] = 0;
        } else {
            std::lock_guard<std::recursive_mutex> g(audioLock);
            WBChannel *ch = &S->audio;
            fill(ch);
            int32_t seq = ch->req + 1;
            MemoryBarrier();
            ch->req = seq;
            SetEvent(evAudioReq);
            HANDLE w[2] = { evAudioResp, helper };
            while (ch->resp != seq) {
                DWORD r = WaitForMultipleObjects(2, w, FALSE, 5000);
                if (ch->resp == seq) break;
                if (r != WAIT_OBJECT_0) {
                    Log("audio request op %d failed (%s)", op, r == WAIT_TIMEOUT ? "timeout" : r == WAIT_OBJECT_0 + 1 ? "helper died" : "wait failed");
                    Lost();
                    return 0;
                }
            }
            collect(ch);
        }
        for (auto &cb : cbs) Replay(cb);
        if (gui) EditorHandle = (HWND)(intptr_t)S->editorHwnd;
        return result;
    }

    void Replay(const WBCallback &cb) {
        int slot = (int)cb.a;
        switch (cb.type) {
        case CB_VOICEKILL: {
            // the plugin says this voice is finished: free our slot first (FL may or may not call our
            // Voice_Kill back, depending on KillHandle), then tell FL
            intptr_t tag;
            {
                std::lock_guard<std::recursive_mutex> g(voiceLock);
                if (slot < 0 || slot >= WB_MAXV || !used[slot]) break;
                tag = flTag[slot]; used[slot] = false; flParams[slot] = nullptr;
            }
            host->Voice_Kill(tag, (BOOL)cb.b);
            break;
        }
        case CB_VOICERELEASE:
            if (slot >= 0 && slot < WB_MAXV && used[slot]) host->Voice_Release(flTag[slot]);
            break;
        case CB_PARAMCHANGED: host->OnParamChanged(HostTag, (int)cb.a, (int)cb.b); break;
        case CB_CONTROLLER: host->OnControllerChanged(HostTag, (intptr_t)cb.a, (intptr_t)cb.b); break;
        case CB_DISPATCH: host->Dispatcher(HostTag, (intptr_t)cb.a, (intptr_t)cb.b, (intptr_t)cb.c); break;
        }
    }

    void DrainQueue() {
        if (broken) return;
        while (S->qHead != S->qTail) {
            WBCallback cb = S->queue[S->qHead];
            MemoryBarrier();
            S->qHead = (S->qHead + 1) % WB_QUEUE;
            Replay(cb);
        }
    }

    void SyncVoiceParams() {
        std::lock_guard<std::recursive_mutex> g(voiceLock);
        for (int i = 0; i < WB_MAXV; i++)
            if (used[i] && flParams[i]) memcpy(S->voices[i].params, flParams[i], 40);
    }

    // ---- TFruityPlug ----
    intptr_t _stdcall Dispatcher(intptr_t ID, intptr_t Index, intptr_t Value) override {
        Remember(ID, Index, Value);
        switch (ID) {
        case FPD_ShowEditor:
            guiThread = GetCurrentThreadId();
            return (intptr_t)Call(true, OP_DISPATCHER, ID, Index, Value);
        case FPD_SetTimeSig:
            return Value ? (intptr_t)Call(true, OP_DISPATCHER, ID, Index, 0, 0, (void *)Value, sizeof(TTimeSigInfo)) : 0;
        // messages that may come from the mixing thread
        case FPD_Flush: case FPD_SetEnabled: case FPD_SetPlaying: case FPD_SongPosChanged: case FPD_SetSamplesPerTick:
            return (intptr_t)Call(OnGuiThread(), OP_DISPATCHER, ID, Index, Value);
        // other scalar messages
        case FPD_ProcessMode: case FPD_SetBlockSize: case FPD_SetSampleRate: case FPD_KillAVoice: case FPD_UseVoiceLevels:
        case FPD_SetPreset: case FPD_SetInternalParam: case FPD_SetNumSends: case FPD_SetFitTime: case FPD_SetIdleTime:
        case FPD_SetFocus: case FPD_Transport: case FPD_MIDIIn: case FPD_RoutingChanged: case FPD_GetParamInfo:
        case FPD_ProjLoaded: case FPD_ShowSettings: case FPD_SetIOLatency: case FPD_PreferredNumIO:
            return (intptr_t)Call(true, OP_DISPATCHER, ID, Index, Value);
        default:
            return 0;   // pointer-carrying or unknown messages: not forwarded
        }
    }
    void _stdcall Idle_Public() override {
        guiThread = GetCurrentThreadId();
        Call(true, OP_IDLE);
        DrainQueue();
    }
    void _stdcall SaveRestoreState(IStream *Stream, BOOL Save) override {
        if (Save) {
            std::vector<uint8_t> data;
            Call(true, OP_SAVE, 0, 0, 0, 0, nullptr, 0, &data);
            if (!data.empty()) { std::lock_guard<std::mutex> g(stateLock); lastState = data; }
            ULONG w;
            if (!data.empty()) Stream->Write(data.data(), (ULONG)data.size(), &w);
        } else {
            std::vector<uint8_t> data;
            uint8_t buf[16384];
            for (;;) {
                ULONG got = 0;
                if (FAILED(Stream->Read(buf, sizeof(buf), &got)) || !got) break;
                data.insert(data.end(), buf, buf + got);
                if (data.size() >= WB_DATA) break;
            }
            { std::lock_guard<std::mutex> g(stateLock); lastState = data; }
            Call(true, OP_LOAD, 0, 0, 0, 0, data.data(), (int)data.size());
            DrainQueue();
        }
    }
    void _stdcall GetName(int Section, int Index, int Value, char *Name) override {
        std::vector<uint8_t> data;
        Call(OnGuiThread(), OP_GETNAME, Section, Index, Value, 0, nullptr, 0, &data);
        size_t n = std::min<size_t>(data.size(), 255);
        if (n) memcpy(Name, data.data(), n);
        Name[n] = 0;
        Name[255] = 0;
    }
    int _stdcall ProcessEvent(int EventID, int EventValue, int Flags) override {
        if (EventID >= 0 && EventID < 16) { std::lock_guard<std::mutex> g(stateLock); events[EventID] = { EventValue, Flags }; }
        return (int)Call(OnGuiThread(), OP_PROCESSEVENT, EventID, EventValue, Flags);
    }
    int _stdcall ProcessParam(int Index, int Value, int RECFlags) override {
        return (int)Call(OnGuiThread(), OP_PROCESSPARAM, Index, Value, RECFlags);
    }
    void _stdcall Eff_Render(PWAV32FS, PWAV32FS, int) override {}
    void _stdcall Gen_Render(PWAV32FS DestBuffer, int &Length) override {
        if (broken) { Length = 0; return; }
        std::lock_guard<std::recursive_mutex> g(audioLock);   // S->render is shared by all render calls
        int total = Length, done = 0;
        float *dst = (float *)DestBuffer;
        while (done < total) {
            int n = std::min(total - done, WB_MAXFRAMES);
            SyncVoiceParams();
            int got = (int)Call(false, OP_RENDER, n);
            if (broken) break;
            const float *src = S->render;
            for (int i = 0; i < got * 2; i++) dst[done * 2 + i] += src[i];
            done += n;
        }
    }
    TVoiceHandle _stdcall TriggerVoice(PVoiceParams VoiceParams, intptr_t SetTag) override {
        if (broken) return FVH_Null;
        int slot = -1;
        for (int attempt = 0; attempt < 2 && slot < 0; attempt++) {
            intptr_t oldest = 0;
            {
                std::lock_guard<std::recursive_mutex> g(voiceLock);
                for (int i = 0; i < WB_MAXV; i++) if (!used[i]) { slot = i; break; }
                if (slot >= 0) {
                    used[slot] = true; flParams[slot] = VoiceParams; flTag[slot] = SetTag; serial[slot] = ++nextSerial;
                    memcpy(S->voices[slot].params, VoiceParams, 40);
                    break;
                }
                int o = 0;
                for (int i = 1; i < WB_MAXV; i++) if ((int32_t)(serial[i] - serial[o]) < 0) o = i;
                oldest = flTag[o];
            }
            if (attempt) return FVH_Null;
            host->Voice_Kill(oldest, TRUE);   // all slots taken: FL kills its oldest voice (calls our Voice_Kill), freeing one
        }
        if (!Call(OnGuiThread(), OP_TRIGGER, slot)) { std::lock_guard<std::recursive_mutex> g(voiceLock); used[slot] = false; return FVH_Null; }
        return slot + 1;
    }
    void _stdcall Voice_Release(TVoiceHandle Handle) override {
        int slot = (int)Handle - 1;
        if (slot >= 0 && slot < WB_MAXV && used[slot]) Call(OnGuiThread(), OP_VRELEASE, slot);
    }
    void _stdcall Voice_Kill(TVoiceHandle Handle) override {
        int slot = (int)Handle - 1;
        if (slot < 0 || slot >= WB_MAXV || !used[slot]) return;
        Call(OnGuiThread(), OP_VKILL, slot);
        std::lock_guard<std::recursive_mutex> g(voiceLock);
        used[slot] = false; flParams[slot] = nullptr;
    }
    int _stdcall Voice_ProcessEvent(TVoiceHandle Handle, int EventID, int EventValue, int Flags) override {
        int slot = (int)Handle - 1;
        if (slot < 0 || slot >= WB_MAXV || !used[slot]) return 0;
        return (int)Call(OnGuiThread(), OP_VEVENT, slot, EventID, EventValue, Flags);
    }
    int _stdcall Voice_Render(TVoiceHandle, PWAV32FS, int &Length) override { Length = 0; return FVR_NoMoreData; }
    void _stdcall NewTick() override { Call(OnGuiThread(), OP_NEWTICK); }
    void _stdcall MIDITick() override {}
    void _stdcall MIDIIn(int &Msg) override { Msg = (int)Call(OnGuiThread(), OP_MIDIIN, (uint32_t)Msg); }
    void _stdcall MsgIn(intptr_t Msg) override { Call(OnGuiThread(), OP_MSGIN, Msg); }
    int _stdcall OutputVoice_ProcessEvent(TOutVoiceHandle, int, int, int) override { return 0; }
    void _stdcall OutputVoice_Kill(TVoiceHandle) override {}
};

TFruityPlug::TFruityPlug() : HostTag(0), Info(nullptr), EditorHandle(0), MonoRender(0) { memset(Reserved, 0, sizeof(Reserved)); }
TFruityPlug::~TFruityPlug() {}
void _stdcall TFruityPlug::DestroyObject() { delete this; }

static Bridge64 *g_last;   // for the test host's diagnostics only
extern "C" __declspec(dllexport) TFruityPlug *_stdcall CreatePlugInstance(TFruityPlugHost *Host, TPluginTag Tag) {
    return g_last = new Bridge64(Host, Tag);
}
// test host: how many voice slots the last instance holds (FL thinks these voices are still playing)
extern "C" __declspec(dllexport) int _stdcall WBDebug_UsedSlots() {
    if (!g_last) return -1;
    int n = 0;
    for (int i = 0; i < WB_MAXV; i++) if (g_last->used[i]) n++;
    return n;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
