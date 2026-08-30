// Windows clipboard -> Scaleform's internal clipboard.
//
// The game's UI is Havok Vision + Scaleform, and its chat box is a GFx
// TextField: Ctrl+C / Ctrl+V there act on Scaleform's own clipboard, which is
// why copy and paste work inside the game while nothing copied in Windows can
// be pasted in. Mirroring one into the other is the whole feature - the player
// still pastes with the game's own key, in any text field it owns.
//
// vScaleformPlugin.vPlugin exports both entry points by name, so unlike the
// rest of the hook this needs no signature scan: GetProcAddress is exact or
// nothing, and a game patch that merely moves code changes nothing here.
//
// Two rules shape everything below:
//   - The clipboard is read on a worker thread, never on the game's pump
//     thread. GetClipboardData against a delayed-render owner (Office, some
//     browsers) sends a message to that owner and blocks until it answers,
//     which on the pump thread is a frozen game.
//   - Nothing is pushed unless GetClipboardSequenceNumber has moved. That is
//     what keeps an in-game Ctrl+C from being clobbered: the client never
//     writes the Windows clipboard, so an in-game copy leaves the sequence
//     alone and the text it put in the GFx clipboard stands.

#include "clipsync.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "gamecmd.h"

namespace {

// 512 characters plus the terminator - the cap the client's own (ANSI-only,
// unreachable) clipboard routine uses. A chat line is far shorter, and GFx
// should not be handed an arbitrary paste buffer.
constexpr size_t kMaxChars = 513;

constexpr DWORD kPollMs = 100;
constexpr DWORD kShutdownWaitMs = 1000;

void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA("[SoulMeterHook] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// VScaleformManager& VScaleformManager::GlobalManager()     - static, no args
// void VScaleformManager::SetClipboardText(const wchar_t*)  - rcx this, rdx text
typedef void*(__fastcall* GlobalManagerFn)();
typedef void(__fastcall* SetClipboardTextFn)(void* self, const wchar_t* text);

enum ResolveState { kUnresolved = 0, kResolved = 1, kFailed = 2 };

volatile LONG g_sfState = kUnresolved;
GlobalManagerFn g_fnGlobalManager = nullptr;
SetClipboardTextFn g_fnSetClipboardText = nullptr;

volatile LONG g_running = 0;
volatile LONG g_enabled = 0;
volatile LONG g_csReady = 0;
HANDLE g_thread = nullptr;
HANDLE g_wake = nullptr;

CRITICAL_SECTION g_cs;

// Written by the watcher, read by the game thread, both under g_cs.
wchar_t g_text[kMaxChars];
size_t g_textLen = 0;
DWORD g_textSeq = 0;
bool g_textValid = false;

// Game thread only, but kept under g_cs so it stays consistent with the
// sequence it refers to.
DWORD g_appliedSeq = 0;
bool g_applied = false;

// GetModuleHandleW only appends ".dll" to a name without an extension, and
// ".vPlugin" is one, so the plugin resolves under its real file name.
bool ResolveScaleform() {
    LONG state = InterlockedCompareExchange(&g_sfState, 0, 0);
    if (state == kResolved)
        return true;
    if (state == kFailed)
        return false;

    HMODULE sf = GetModuleHandleW(L"vScaleformPlugin.vPlugin");
    if (!sf)
        return false; // Loaded during engine init; not a failure yet.

    GlobalManagerFn gm = (GlobalManagerFn)GetProcAddress(
        sf, "?GlobalManager@VScaleformManager@@SAAEAV1@XZ");
    SetClipboardTextFn sc = (SetClipboardTextFn)GetProcAddress(
        sf, "?SetClipboardText@VScaleformManager@@QEAAXPEB_W@Z");
    if (!gm || !sc) {
        // The plugin is there but is not this build of Scaleform, and retrying
        // will not change that.
        InterlockedExchange(&g_sfState, kFailed);
        Log("clipsync disabled: vScaleformPlugin.vPlugin is missing an entry point");
        return false;
    }

    g_fnGlobalManager = gm;
    g_fnSetClipboardText = sc;
    InterlockedExchange(&g_sfState, kResolved);
    Log("clipsync armed: GlobalManager=%p SetClipboardText=%p", (void*)gm, (void*)sc);
    return true;
}

// Chat is one line, so newlines and tabs become single spaces rather than
// cutting the paste short the way the client's own routine does. Surrogate
// pairs are kept whole - GFx stores UTF-8, so half a pair would reach it as an
// invalid code unit.
size_t Sanitise(const wchar_t* src, size_t maxIn, wchar_t* out, size_t outCap) {
    size_t n = 0;
    for (size_t i = 0; i < maxIn && n + 1 < outCap; i++) {
        wchar_t c = src[i];
        if (c == 0)
            break;

        if (c == L'\r')
            continue; // CRLF collapses onto the space the LF becomes.
        if (c == L'\n' || c == L'\t')
            c = L' ';
        else if (c < 0x20 || c == 0x7F)
            continue;

        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= maxIn || n + 2 >= outCap)
                break;
            wchar_t lo = src[i + 1];
            if (lo < 0xDC00 || lo > 0xDFFF)
                continue;
            out[n++] = c;
            out[n++] = lo;
            i++;
            continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF)
            continue;

        out[n++] = c;
    }

    while (n && out[n - 1] == L' ')
        n--;
    out[n] = 0;
    return n;
}

// True once `seq` has been dealt with, whether or not it held text worth
// caching. False only when the clipboard could not be opened, so the same
// sequence is retried on the next tick.
bool CacheClipboardText(DWORD seq) {
    // A CF_TEXT-only producer is covered: Windows synthesises CF_UNICODETEXT.
    // Anything else - files, a bitmap - leaves the cache alone, so whatever was
    // last mirrored still pastes.
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return true;

    if (!OpenClipboard(nullptr))
        return false;

    wchar_t staging[kMaxChars];
    size_t staged = 0;
    bool sawText = false;

    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        SIZE_T bytes = GlobalSize(h);
        const wchar_t* src = (const wchar_t*)GlobalLock(h);
        if (src) {
            if (bytes >= sizeof(wchar_t)) {
                // The text is a terminated run inside a block that may be
                // larger; GlobalSize bounds a producer that omits the
                // terminator.
                size_t maxIn = bytes / sizeof(wchar_t);
                if (maxIn > kMaxChars * 4)
                    maxIn = kMaxChars * 4; // The cap truncates anyway.
                staged = Sanitise(src, maxIn, staging, kMaxChars);
                sawText = true;
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();

    if (!sawText || staged == 0)
        return true;

    EnterCriticalSection(&g_cs);
    memcpy(g_text, staging, (staged + 1) * sizeof(wchar_t));
    g_textLen = staged;
    g_textSeq = seq;
    g_textValid = true;
    LeaveCriticalSection(&g_cs);

    Log("clipsync: cached %zu chars (seq %lu)", staged, (unsigned long)seq);
    return true;
}

// Separated so the __try frame holds nothing that needs unwinding.
bool PushToScaleform(const wchar_t* text) {
    __try {
        // SetClipboardText null-checks its own GFx clipboard, so a call made
        // before the UI exists is a no-op rather than a fault.
        void* mgr = g_fnGlobalManager();
        if (mgr)
            g_fnSetClipboardText(mgr, text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_sfState, kFailed);
        Log("clipsync disabled: SetClipboardText faulted");
        return false;
    }
}

DWORD WINAPI WatcherThread(LPVOID) {
    DWORD lastSeq = 0;
    bool haveSeq = false;

    while (InterlockedCompareExchange(&g_running, 0, 0)) {
        WaitForSingleObject(g_wake, kPollMs);
        if (!InterlockedCompareExchange(&g_enabled, 0, 0))
            continue;

        // The window can be recreated long after the command pipe connected -
        // a borderless/fullscreen toggle does it - and GameCmdInit is only
        // reached while that pipe is disconnected.
        GameCmdEnsureArmed();

        // Reads a counter; does not open the clipboard.
        DWORD seq = GetClipboardSequenceNumber();
        if (haveSeq && seq == lastSeq)
            continue;
        if (!CacheClipboardText(seq))
            continue;
        lastSeq = seq;
        haveSeq = true;

        // Best effort: a post that finds no window yet is not a loss, because
        // the apply is driven by the cached sequence and the next activation
        // picks it up.
        GameCmdPostLocal(SMH_CMD_CLIP_APPLY, 0);
    }
    return 0;
}

} // namespace

void ClipSyncSetEnabled(bool enabled) {
    if (enabled && InterlockedCompareExchange(&g_csReady, 1, 0) == 0)
        InitializeCriticalSection(&g_cs);

    if (enabled && InterlockedCompareExchange(&g_running, 1, 0) == 0) {
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_wake)
            g_thread = CreateThread(nullptr, 0, WatcherThread, nullptr, 0, nullptr);
        if (!g_thread) {
            if (g_wake) {
                CloseHandle(g_wake);
                g_wake = nullptr;
            }
            InterlockedExchange(&g_running, 0);
            Log("clipsync: watcher could not start");
            return;
        }
    }

    // Set last: nothing may enter the critical section before it exists.
    InterlockedExchange(&g_enabled, enabled ? 1 : 0);
    if (g_wake)
        SetEvent(g_wake);
    Log("clipsync: %s", enabled ? "enabled" : "disabled");
}

void ClipSyncApplyOnGameThread() {
    if (!InterlockedCompareExchange(&g_enabled, 0, 0))
        return;
    if (!ResolveScaleform())
        return;

    wchar_t local[kMaxChars];
    DWORD seq = 0;

    EnterCriticalSection(&g_cs);
    bool fresh = g_textValid && (!g_applied || g_textSeq != g_appliedSeq);
    if (fresh) {
        memcpy(local, g_text, (g_textLen + 1) * sizeof(wchar_t));
        seq = g_textSeq;
    }
    LeaveCriticalSection(&g_cs);
    if (!fresh)
        return;

    // Called with the lock released, so foreign code never runs under it. A
    // fault latches the resolve state, so this cannot retry forever.
    if (!PushToScaleform(local))
        return;

    EnterCriticalSection(&g_cs);
    g_appliedSeq = seq;
    g_applied = true;
    LeaveCriticalSection(&g_cs);
}

void ClipSyncOnForeground() {
    ClipSyncApplyOnGameThread();
}

void ClipSyncShutdown() {
    if (InterlockedExchange(&g_running, 0) == 0)
        return;

    InterlockedExchange(&g_enabled, 0);
    SetEvent(g_wake);

    // Bounded: the watcher may be inside GetClipboardData, which is not ours to
    // interrupt. On a timeout the thread and its objects are left alone -
    // freeing them under a live thread is worse than leaking at process exit,
    // which is the only time this runs.
    if (WaitForSingleObject(g_thread, kShutdownWaitMs) == WAIT_OBJECT_0) {
        CloseHandle(g_thread);
        CloseHandle(g_wake);
        g_thread = nullptr;
        g_wake = nullptr;
    }
}
