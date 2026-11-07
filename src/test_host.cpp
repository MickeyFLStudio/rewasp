// Offline test: load a Fruity generator, play a note, write out.wav, report level & pitch.
// usage: test_host.exe <plugin.dll> [note] [out.wav] [state.bin]
#include "flhost.h"
#include <tlhelp32.h>
#include <shlwapi.h>
#include "ipc.h"
#pragma comment(lib, "shlwapi.lib")
#include <stdlib.h>
#include <algorithm>

// WB_TEST_KILL=1: after the first note, kill the bridge's helper process and check that the bridge
// brings it back by itself (the hostWnd timer needs this thread's message queue pumped, like FL does)
static DWORD FindHelper() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e = { sizeof(e) }; DWORD found = 0;
    if (Process32FirstW(snap, &e)) do {
        if (e.th32ParentProcessID == GetCurrentProcessId() && !_wcsicmp(e.szExeFile, L"WaspHost32.exe")) found = e.th32ProcessID;
    } while (Process32NextW(snap, &e));
    CloseHandle(snap);
    return found;
}
static void Pump(DWORD ms) {
    DWORD until = GetTickCount() + ms;
    while ((int)(until - GetTickCount()) > 0) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(20);
    }
}

static void WriteWav(const char *path, const std::vector<float> &s, int sr) {
    FILE *f; fopen_s(&f, path, "wb"); if (!f) return;
    int data = (int)s.size() * 4, fmt = 3, ch = 2, bits = 32, br = sr * 8, ba = 8, sz = 36 + data, fl = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&sz, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(s.data(), 4, s.size(), f); fclose(f);
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) { printf("usage: test_host plugin.dll [note] [out.wav]\n"); return 1; }
    int note = argc > 2 ? _wtoi(argv[2]) : 69;
    char out[MAX_PATH] = "out.wav";
    if (argc > 3) WideCharToMultiByte(CP_ACP, 0, argv[3], -1, out, MAX_PATH, 0, 0);

    CoInitialize(nullptr);
    FLHost host;
    host.SetSampleRate(44100);
    host.SetBlockSize(512);
    if (!host.Load(argv[1])) { printf("load failed\n"); return 2; }
    if (argc > 4) {   // restore a saved plugin state (e.g. extracted from an .flp)
        FILE *sf = _wfopen(argv[4], L"rb");
        std::vector<char> st; if (sf) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, sf)) > 0) st.insert(st.end(), b, b + n); fclose(sf); }
        IStream *is = nullptr; CreateStreamOnHGlobal(nullptr, TRUE, &is);
        ULONG w; is->Write(st.data(), (ULONG)st.size(), &w); LARGE_INTEGER z = {}; is->Seek(z, STREAM_SEEK_SET, nullptr);
        host.plug->SaveRestoreState(is, FALSE); is->Release();
        printf("restored %d state bytes\n", (int)st.size());
    }
    TFruityPlugInfo *in = host.plug->Info;
    printf("name=%s short=%s flags=0x%x params=%d defpoly=%d sdk=%d\n", in->LongName, in->ShortName, in->Flags, in->NumParams, in->DefPoly, in->SDKVersion);
    char name[512];
    for (int i = 0; i < in->NumParams && i < 200; i++) {
        name[0] = 0;
        host.plug->GetName(FPN_Param, i, 0, name);
        int v = host.plug->ProcessParam(i, 0, REC_GetValue);
        printf("  p%-3d %-28s = %d\n", i, name, v);
    }

    const int sr = 44100, block = 512;
    std::vector<float> all, buf(block * 2);
    auto run = [&](double secs) {
        for (int n = 0; n < (int)(secs * sr / block); n++) { host.Render(buf.data(), block); all.insert(all.end(), buf.begin(), buf.end()); }
    };
    run(0.05);
    host.NoteOn(note, 0.8f);
    size_t onStart = all.size();
    run(1.0);
    size_t onEnd = all.size();
    host.NoteOff(note);
    run(1.5);

    double peak = 0, rms = 0; int cross = 0; float prev = 0;
    for (size_t i = onStart; i < onEnd; i += 2) {
        float x = all[i]; peak = std::max(peak, (double)fabs(x)); rms += x * x;
        if (i > onStart + sr / 2 && prev < 0 && x >= 0) cross++;   // count over the last half second
        prev = x;
    }
    rms = sqrt(rms / ((onEnd - onStart) / 2));
    double tailRms = 0;
    for (size_t i = all.size() - sr; i < all.size(); i += 2) tailRms += all[i] * all[i];
    tailRms = sqrt(tailRms / (sr / 2));
    printf("note %d: peak=%.3f rms=%.4f approx freq=%.1f Hz (expect %.1f)  tail rms=%.5f  voices left=%s\n",
           note, peak, rms, cross / ((onEnd - onStart) / 2.0 / sr - 0.5), 440 * pow(2, (note - 69) / 12.0), tailRms, "?");
    if (GetEnvironmentVariableW(L"WB_TEST_NOTES", nullptr, 0)) {   // 300 short notes: more than the bridge has slots
        // held, overlapping notes: far more than Wasp's 4 voices, so it has to steal voices
        for (int i = 0; i < 300; i++) { host.NoteOn(48 + i % 24, 0.8f); run(0.02); }
        host.AllNotesOff(); run(1.0);
        size_t s3 = all.size();
        host.NoteOn(note, 0.8f); run(0.5); host.NoteOff(note); run(0.3);
        double peak3 = 0;
        for (size_t i = s3; i < all.size(); i++) peak3 = std::max(peak3, (double)fabs(all[i]));
        int slots = -1;
        wchar_t base[MAX_PATH]; wcscpy_s(base, argv[1]); PathStripPathW(base);
        if (HMODULE hm = GetModuleHandleW(base))
            if (auto f = (int(_stdcall *)())GetProcAddress(hm, "WBDebug_UsedSlots")) slots = f();
        printf("after 300 held notes: voice slots still held=%d (of %d), test note peak=%.3f -> %s\n", slots, WB_MAXV, peak3,
               slots < 0 ? "no slot info in this build" : slots <= 8 && peak3 > 0.01 ? "OK" : "LEAK");
    }
    wchar_t mode[16] = L"";
    if (GetEnvironmentVariableW(L"WB_TEST_KILL", mode, 16)) {   // "1" = kill, "hang" = freeze every helper thread
        Pump(2500);               // let the bridge take a snapshot of the patch first
        host.plug->ProcessParam(0, 777, REC_UpdateValue);   // a tweak the restored helper must still show
        Pump(2500);
        int before = host.plug->ProcessParam(0, 0, REC_GetValue);
        DWORD pid = FindHelper();
        if (!pid) printf("helper process not found\n");
        else if (!wcscmp(mode, L"hang")) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 t = { sizeof(t) }; int n = 0;
            if (Thread32First(snap, &t)) do {
                if (t.th32OwnerProcessID == pid) if (HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, t.th32ThreadID)) { SuspendThread(th); CloseHandle(th); n++; }
            } while (Thread32Next(snap, &t));
            CloseHandle(snap);
            printf("froze the helper (pid %lu, %d threads)\n", pid, n);
        } else {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (h) { TerminateProcess(h, 1); CloseHandle(h); printf("killed the helper (pid %lu)\n", pid); }
        }
        Pump(300);
        run(0.3);                 // renders hit the dead helper: the bridge notices
        Pump(8000);               // GUI-thread ticks: the bridge restarts it (a frozen helper is noticed after 5 s)
        size_t s2 = all.size();
        host.NoteOn(note, 0.8f); run(1.0); host.NoteOff(note); run(0.3);
        double peak2 = 0;
        for (size_t i = s2; i < all.size(); i++) peak2 = std::max(peak2, (double)fabs(all[i]));
        int after = host.plug->ProcessParam(0, 0, REC_GetValue);
        printf("after losing the helper: peak=%.3f, param 0 was %d now %d -> %s\n", peak2, before, after,
               peak2 > 0.01 && after == before ? "RECOVERED" : "FAILED");
    }
    WriteWav(out, all, sr);
    printf("wrote %s\n", out);
    host.Unload();
    return 0;
}
