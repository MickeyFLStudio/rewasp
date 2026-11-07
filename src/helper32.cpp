// WaspHost32.exe: 32-bit helper process. Loads the original 32-bit Wasp.dll and
// serves requests from the 64-bit native plugin (native64.cpp) over shared memory.
// usage: WaspHost32.exe <id> <path to 32-bit plugin dll> <parent pid>
#include "flhost.h"
#include "ipc.h"

static WBShared *S;
static HANDLE evAudioReq, evAudioResp, parentProc;
static volatile bool quitting = false;
static DWORD guiThreadId;

// Knob right-click menu. Wasp shows it from this process as Image-Line's own popup window
// (TQuickPopupMenuWindow, not a Win32 menu). Keys go to the keyboard-focus window, which is
// FL's, so the menu never saw E (edit events), A (automation clip)... Activating the popup
// closes it at once, but Wasp's editor sits inside FL's window, so our thread shares FL's
// input state: we move just the keyboard focus to the editor while the popup is up, and give
// it back to FL's control afterwards. The popup also closes with Wasp's window.
static HWND callWnd, focusReturn, menuPopup;
static const UINT_PTR MENU_TIMER = 1;
static DWORD menuStart;
static bool menuFocused;

static HWND FindMenuPopup() {
    HWND found = nullptr;
    EnumThreadWindows(guiThreadId, [](HWND h, LPARAM l) -> BOOL {
        wchar_t c[64];
        GetClassNameW(h, c, 64);
        if (IsWindowVisible(h) && (!wcscmp(c, L"TQuickPopupMenuWindow") || !wcscmp(c, L"#32768"))) {
            *(HWND *)l = h;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&found);
    return found;
}

static void CloseMenuPopup(HWND popup) {
    wchar_t c[64];
    GetClassNameW(popup, c, 64);
    if (!wcscmp(c, L"#32768")) { EndMenu(); return; }
    PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0x00010001);   // to the popup only (never to FL)
    PostMessageW(popup, WM_KEYUP, VK_ESCAPE, 0xC0010001);
}

static void MenuDone() {
    KillTimer(callWnd, MENU_TIMER);
    // hand the keyboard focus back, but only if it is still ours (the menu may have opened an FL window)
    DWORD pid = 0;
    GetWindowThreadProcessId(GetFocus(), &pid);
    if (menuFocused && focusReturn && IsWindow(focusReturn) && pid == GetCurrentProcessId()) SetFocus(focusReturn);
    focusReturn = menuPopup = nullptr;
    menuFocused = false;
}

// Wasp is about to show its menu (it asks FL for the entries first)
static void MenuOpening() {
    focusReturn = GetFocus();   // FL's control that has the keyboard (shared input state, see above)
    menuPopup = nullptr;
    menuFocused = false;
    menuStart = GetTickCount();
    SetTimer(callWnd, MENU_TIMER, 15, nullptr);   // watched from here (timers run inside the popup's loop)
}

static void MenuTick();   // below: needs `host`
static thread_local WBChannel *tlsChan = nullptr;

class HelperHost : public FLHost {
public:
    void PushCb(int type, int64_t a, int64_t b = 0, int64_t c = 0) {
        WBCallback cb = { type, 0, a, b, c };
        if (tlsChan) {
            if (tlsChan->nCb < WB_MAXCB) tlsChan->cb[tlsChan->nCb++] = cb;
            return;
        }
        // raised outside a request (user turned a knob): queue for the 64-bit side's Idle
        int t = S->qTail, next = (t + 1) % WB_QUEUE;
        if (next == S->qHead) return;   // full, drop
        S->queue[t] = cb;
        MemoryBarrier();
        S->qTail = next;
    }

    // Synchronous call into FL's dispatcher on the 64-bit side (see WBHostCall in ipc.h)
    // GUI thread only: from the audio thread FL's GUI thread may itself be waiting on us.
    int64_t HostCall(intptr_t id, intptr_t index, intptr_t value, WBHostCall *reply = nullptr, const char *text = nullptr) {
        if (reply) memset(reply, 0, sizeof(*reply));
        HWND hw = (HWND)(intptr_t)S->hostHwnd;
        if (!hw || GetCurrentThreadId() != guiThreadId) return 0;
        int f = -1;
        for (int i = 0; i < WB_GUIFRAMES && f < 0; i++)
            if (InterlockedCompareExchange((volatile LONG *)&S->hostBusy[i], 1, 0) == 0) f = i;
        if (f < 0) { HostLog("host calls nested too deep (id %d)", (int)id); return 0; }
        WBHostCall *c = &S->host[f];
        c->id = id; c->index = index; c->value = value; c->result = 0; c->hasEntry = 0;
        c->textIn = text != nullptr;
        if (text) strncpy_s(c->entryName, text, _TRUNCATE);
        MemoryBarrier();
        DWORD_PTR ok = 0;
        if (!SendMessageTimeoutW(hw, WM_WB_HOST, f, 0, SMTO_NORMAL, 30000, &ok) || !ok)
            HostLog("host call id %d failed (err %lu)", (int)id, GetLastError());
        MemoryBarrier();
        int64_t r = c->result;
        if (reply) *reply = *c;
        S->hostBusy[f] = 0;
        return r;
    }

    void _stdcall Voice_Kill(intptr_t Sender, BOOL KillHandle) override {
        int slot = (int)Sender - 1;
        if (slot >= 0 && slot < WB_MAXV) S->voices[slot].active = 0;   // finished: its handle may be reused any moment
        PushCb(CB_VOICEKILL, slot, KillHandle);
    }
    void _stdcall Voice_Release(intptr_t Sender) override { PushCb(CB_VOICERELEASE, Sender - 1); }
    int _stdcall Voice_ProcessEvent(intptr_t Sender, intptr_t EventID, intptr_t, intptr_t) override {
        int slot = (int)Sender - 1;
        float f;
        switch (EventID) {
        case FPV_GetLength: return -1;
        case FPV_GetColor: return 0;
        case FPV_GetVelocity:   // InitLevels.Vol sits at offset 4 in both the old and new layouts
            f = (slot >= 0 && slot < WB_MAXV) ? *(float *)(S->voices[slot].params + 4) : 0.8f;
            return *(int *)&f;
        case FPV_GetRelVelocity: f = 0.5f; return *(int *)&f;
        case FPV_GetRelTime: f = 1.0f; return *(int *)&f;
        default: return 0;
        }
    }
    // Raised by the user turning a knob (outside any request): post it straight to FL instead of
    // waiting for the next Idle, so recorded automation lands where the move happened.
    void PostOrPush(UINT msg, int type, int64_t index, int64_t value) {
        HWND hw = (HWND)(intptr_t)S->hostHwnd;
        if (!tlsChan && hw && PostMessageW(hw, msg, (WPARAM)index, (LPARAM)value)) return;
        PushCb(type, index, value);
    }
    void _stdcall OnParamChanged(TPluginTag, int Index, int Value) override { PostOrPush(WM_WB_PARAM, CB_PARAMCHANGED, Index, Value); }
    void _stdcall OnControllerChanged(TPluginTag, intptr_t Index, intptr_t Value) override { PostOrPush(WM_WB_CTRL, CB_CONTROLLER, Index, Value); }
    void _stdcall OnHint(TPluginTag, char *Text) override { if (Text) HostCall(WB_HOST_ONHINT, 0, 0, nullptr, Text); }
    intptr_t _stdcall Dispatcher(TPluginTag Sender, intptr_t ID, intptr_t Index, intptr_t Value) override {
        switch (ID) {
        // scalar messages FL itself should act on
        case FHD_EditorResized: case FHD_NamesChanged: case FHD_ActivateMIDI: case FHD_WantMIDIInput:
        case FHD_KillAutomation: case FHD_SetNumPresets: case FHD_WantIdle: case FHD_SetNewColor:
        case FHD_KillIntCtrl: case FHD_SetNumParams: case FHD_SetLatency: case FHD_SetThreadSafe:
        case FHD_SmartDisable: case FHD_SetDirty: case FHD_ShowEditor:
            PushCb(CB_DISPATCH, ID, Index, Value);
            return 0;
        // knob right-click menu (automation, controller links...): FL must answer now
        case FHD_ParamMenu: case FHD_GetParamMenuFlags:
            return (intptr_t)HostCall(ID, Index, Value);
        case FHD_OnHint_Direct:
            return Value ? (intptr_t)HostCall(ID, Index, 0, nullptr, (const char *)Value) : 0;
        case FHD_GetMainFormHandle:   // FL's main window, so Wasp's dialogs open on top of FL (HWNDs fit in 32 bits)
            return Value ? 0 : (intptr_t)HostCall(ID, Index, 0);
        case FHD_GetParamMenuEntry: {
            // Wasp reads entries 0,1,2... until null; each needs its own lasting buffer
            static TParamMenuEntry entries[64];
            static char names[64][256];
            if (Value < 0 || Value >= 64) return 0;
            if (Value == 0) MenuOpening();
            WBHostCall reply;
            HostCall(ID, Index, Value, &reply);
            if (!reply.hasEntry) return 0;
            strncpy_s(names[Value], reply.entryName, _TRUNCATE);
            entries[Value].Name = names[Value];
            entries[Value].Flags = reply.entryFlags;
            return (intptr_t)&entries[Value];
        }
        default:
            return FLHost::Dispatcher(Sender, ID, Index, Value);
        }
    }
};

static HelperHost host;

static void MenuTick() {
    HWND popup = FindMenuPopup();
    if (!popup) {
        if (menuPopup || GetTickCount() - menuStart > 1500) MenuDone();   // closed, or never appeared
        return;
    }
    menuPopup = popup;
    HWND ed = host.plug ? host.plug->EditorHandle : nullptr;
    if (!ed || !IsWindowVisible(ed)) { CloseMenuPopup(popup); return; }   // Wasp's window went away
    if (!menuFocused) { SetFocus(ed); menuFocused = true; }   // the popup's keys now reach this thread
}

static void Serve(WBChannel *ch, bool gui) {
    WBChannel *outer = tlsChan;   // GUI requests can nest (see ipc.h), so restore on the way out
    tlsChan = ch;
    ch->nCb = 0;
    int64_t r = 0;
    TFruityPlug *p = host.plug;
    int slot = (int)ch->a;
    bool slotOk = slot >= 0 && slot < WB_MAXV;
    switch (ch->op) {
    case OP_DISPATCHER: {
        intptr_t id = (intptr_t)ch->a, idx = (intptr_t)ch->b, val = (intptr_t)ch->c;
        if (id == FPD_SetTimeSig) val = (intptr_t)ch->data;
        if (id == FPD_SetSampleRate) host.sampleRate = (double)val;
        if (id == FPD_SetBlockSize) { host.SetBlockSize((int)std::min<intptr_t>(val, WB_MAXFRAMES)); r = 0; break; }
        if (id == FPD_ShowEditor && !val) if (HWND pm = FindMenuPopup()) CloseMenuPopup(pm);   // don't leave its menu behind
        std::lock_guard<std::recursive_mutex> g(host.mix);
        r = p->Dispatcher(id, idx, val);
        break;
    }
    case OP_IDLE: p->Idle_Public(); break;
    case OP_SAVE: {
        IStream *s = nullptr;
        ch->dataLen = 0;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &s) == S_OK) {
            p->SaveRestoreState(s, TRUE);
            STATSTG st; s->Stat(&st, STATFLAG_NONAME);
            ULONG n = (ULONG)std::min<ULONGLONG>(st.cbSize.QuadPart, WB_DATA), got = 0;
            LARGE_INTEGER zero = {}; s->Seek(zero, STREAM_SEEK_SET, nullptr);
            s->Read(ch->data, n, &got);
            ch->dataLen = (int32_t)got;
            s->Release();
        }
        break;
    }
    case OP_LOAD: {
        IStream *s = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &s) == S_OK) {
            ULONG w; s->Write(ch->data, ch->dataLen, &w);
            LARGE_INTEGER zero = {}; s->Seek(zero, STREAM_SEEK_SET, nullptr);
            std::lock_guard<std::recursive_mutex> g(host.mix);
            p->SaveRestoreState(s, FALSE);
            s->Release();
        }
        break;
    }
    case OP_GETNAME: {
        char name[1024] = {};
        p->GetName((int)ch->a, (int)ch->b, (int)ch->c, name);
        name[sizeof(name) - 1] = 0;
        ch->dataLen = (int32_t)strlen(name) + 1;
        memcpy(ch->data, name, ch->dataLen);
        break;
    }
    case OP_PROCESSEVENT: {
        std::lock_guard<std::recursive_mutex> g(host.mix);
        if (ch->a == FPE_Tempo) { int v = (int)ch->b; host.tempo = *(float *)&v; }
        r = p->ProcessEvent((int)ch->a, (int)ch->b, (int)ch->c);
        break;
    }
    case OP_PROCESSPARAM: r = p->ProcessParam((int)ch->a, (int)ch->b, (int)ch->c); break;
    case OP_RENDER: {
        int n = (int)std::min<int64_t>(ch->a, WB_MAXFRAMES);
        memset(S->render, 0, sizeof(float) * 2 * n);
        std::lock_guard<std::recursive_mutex> g(host.mix);
        int len = n;
        p->Gen_Render((PWAV32FS)S->render, len);
        r = len;
        static const bool debug = GetEnvironmentVariableA("WB_DEBUG", nullptr, 0) > 0;
        if (debug) {   // level log for diagnostics: one line per ~2 s of audio that had signal
            static float peak = 0; static int64_t frames = 0;
            for (int i = 0; i < len * 2; i++) peak = std::max(peak, fabsf(S->render[i]));
            frames += len;
            if (frames >= (int64_t)host.sampleRate * 2) { if (peak > 0) HostLog("render peak %.3f", peak); peak = 0; frames = 0; }
        }
        break;
    }
    case OP_TRIGGER: {
        if (!slotOk) break;
        std::lock_guard<std::recursive_mutex> g(host.mix);
        TVoiceHandle h = p->TriggerVoice((PVoiceParams)S->voices[slot].params, slot + 1);
        if (h != FVH_Null) {
            // Wasp has a few voice objects and reuses one for a new note without saying so: the slot
            // that still points at it is dead now (else its release/kill would hit the new note, and
            // FL's voice list would fill up)
            for (int i = 0; i < WB_MAXV; i++)
                if (i != slot && S->voices[i].active && S->voices[i].handle == (int32_t)h) { S->voices[i].active = 0; host.PushCb(CB_VOICEKILL, i, 1); }
            S->voices[slot].handle = (int32_t)h; S->voices[slot].active = 1; r = 1;
        }
        break;
    }
    case OP_VRELEASE:
        if (slotOk && S->voices[slot].active) { std::lock_guard<std::recursive_mutex> g(host.mix); p->Voice_Release(S->voices[slot].handle); }
        break;
    case OP_VKILL:
        if (slotOk && S->voices[slot].active) {
            std::lock_guard<std::recursive_mutex> g(host.mix);
            S->voices[slot].active = 0;
            p->Voice_Kill(S->voices[slot].handle);
        }
        break;
    case OP_VEVENT:
        if (slotOk && S->voices[slot].active) r = p->Voice_ProcessEvent(S->voices[slot].handle, (int)ch->b, (int)ch->c, (int)ch->d);
        break;
    case OP_NEWTICK: { std::lock_guard<std::recursive_mutex> g(host.mix); p->NewTick(); break; }
    case OP_MIDIIN: { int m = (int)ch->a; p->MIDIIn(m); r = m; break; }
    case OP_MSGIN: p->MsgIn((intptr_t)ch->a); break;
    case OP_DESTROY:
        host.Unload();
        quitting = true;
        break;
    }
    ch->result = r;
    tlsChan = outer;
    if (gui && host.plug) S->editorHwnd = (int64_t)(intptr_t)host.plug->EditorHandle;
    MemoryBarrier();
    if (!gui) { ch->resp = ch->req; SetEvent(evAudioResp); }
}

static LRESULT CALLBACK CallWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == MENU_TIMER) { MenuTick(); return 0; }
    if (m == WM_WB_CALL) {
        if (w < WB_GUIFRAMES) Serve(&S->gui[w], true);
        if (quitting) PostQuitMessage(0);
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI AudioThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE w[2] = { evAudioReq, parentProc };
    while (!quitting) {
        DWORD r = WaitForMultipleObjects(2, w, FALSE, 1000);
        if (r == WAIT_OBJECT_0 + 1) { quitting = true; break; }
        if (S->audio.req != S->audio.resp) Serve(&S->audio, false);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 4) return 1;
    std::wstring id = argv[1], dll = argv[2];
    DWORD ppid = (DWORD)_wtoi(argv[3]);
    auto name = [&](const wchar_t *suffix) { return L"Local\\WaspBridge." + id + suffix; };

    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name(L".mem").c_str());
    if (!map) return 2;
    S = (WBShared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WBShared));
    if (!S || S->magic != WB_MAGIC) return 3;
    evAudioReq = OpenEventW(EVENT_ALL_ACCESS, FALSE, name(L".audioReq").c_str());
    evAudioResp = OpenEventW(EVENT_ALL_ACCESS, FALSE, name(L".audioResp").c_str());
    HANDLE evReady = OpenEventW(EVENT_ALL_ACCESS, FALSE, name(L".ready").c_str());
    parentProc = OpenProcess(SYNCHRONIZE, FALSE, ppid);
    if (!evAudioReq || !evAudioResp || !evReady || !parentProc) return 4;

    OleInitialize(nullptr);
    guiThreadId = GetCurrentThreadId();
    if (!host.Load(dll.c_str())) { S->error = 1; SetEvent(evReady); return 5; }
    TFruityPlugInfo *in = host.plug->Info;
    S->sdk = in->SDKVersion; S->flags = in->Flags; S->numParams = in->NumParams; S->defPoly = in->DefPoly;
    S->numOutCtrls = in->NumOutCtrls; S->numOutVoices = in->NumOutVoices;
    strncpy_s(S->longName, in->LongName ? in->LongName : "", _TRUNCATE);
    strncpy_s(S->shortName, in->ShortName ? in->ShortName : "", _TRUNCATE);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = CallWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WaspBridgeCall";
    RegisterClassW(&wc);
    callWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    S->helperHwnd = (int64_t)(intptr_t)callWnd;
    S->ready = 1;
    MemoryBarrier();
    SetEvent(evReady);

    HANDLE at = CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);

    // GUI thread: pump the plugin's window messages; GUI requests arrive as sent messages
    while (!quitting) {
        DWORD r = MsgWaitForMultipleObjects(1, &parentProc, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quitting = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    quitting = true;
    SetEvent(evAudioReq);
    WaitForSingleObject(at, 2000);
    if (host.plug) host.Unload();
    // Wasp.dll faults in its own shutdown code when the process ends normally (every closed Wasp
    // channel left a crash report behind); nothing of ours is left to run, so end without it
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
