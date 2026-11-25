// The game's system calls. Both consoles run Horizon, so most of them are the host's own and go through as they are: threads, locks,
// events, time, and the messages to the system's services. Answered here are the ones where this program stands between the game and
// the system: the heap (the host's is this program's own), the ends (exit, break), and the debug string. Every number is counted, and
// its first uses logged with where in the game they came from, which is the map of what a game needs.
// Runs on the game's threads: no C library, no thread-local storage (see log.c).
#include <switch.h>

#include "nx2.h"

static u8 *heap_base;
static u64 heap_max, heap_now;
void nx2_heap_set(void *base, u64 size) { heap_base = base, heap_max = size; }
// A Switch 2 game's (main.c): no block is set aside. The kernel's heap is the host's part alone, `host` bytes from `base`, and the
// game's SetHeapSize makes it larger by what the game asks: the game's heap is what comes after the host's part. `most` is what
// the game is told it has, and the most it may ask as a heap.
static u64 live_base, live_host;
void nx2_heap_live(u64 base, u64 host, u64 most) { live_base = base, live_host = host, heap_max = most; }

static u32 counts[0xC0];

static const char *name_of(u32 n) {
    switch (n) {
    case 0x01: return "SetHeapSize";
    case 0x02: return "SetMemoryPermission";
    case 0x03: return "SetMemoryAttribute";
    case 0x04: return "MapMemory";
    case 0x05: return "UnmapMemory";
    case 0x06: return "QueryMemory";
    case 0x07: return "ExitProcess";
    case 0x08: return "CreateThread";
    case 0x09: return "StartThread";
    case 0x0A: return "ExitThread";
    case 0x0B: return "SleepThread";
    case 0x0C: return "GetThreadPriority";
    case 0x0D: return "SetThreadPriority";
    case 0x0E: return "GetThreadCoreMask";
    case 0x0F: return "SetThreadCoreMask";
    case 0x10: return "GetCurrentProcessorNumber";
    case 0x11: return "SignalEvent";
    case 0x12: return "ClearEvent";
    case 0x13: return "MapSharedMemory";
    case 0x14: return "UnmapSharedMemory";
    case 0x15: return "CreateTransferMemory";
    case 0x16: return "CloseHandle";
    case 0x17: return "ResetSignal";
    case 0x18: return "WaitSynchronization";
    case 0x19: return "CancelSynchronization";
    case 0x1A: return "ArbitrateLock";
    case 0x1B: return "ArbitrateUnlock";
    case 0x1C: return "WaitProcessWideKeyAtomic";
    case 0x1D: return "SignalProcessWideKey";
    case 0x1E: return "GetSystemTick";
    case 0x1F: return "ConnectToNamedPort";
    case 0x21: return "SendSyncRequest";
    case 0x22: return "SendSyncRequestWithUserBuffer";
    case 0x24: return "GetProcessId";
    case 0x25: return "GetThreadId";
    case 0x26: return "Break";
    case 0x27: return "OutputDebugString";
    case 0x28: return "ReturnFromException";
    case 0x29: return "GetInfo";
    case 0x2C: return "MapPhysicalMemory";
    case 0x2D: return "UnmapPhysicalMemory";
    case 0x32: return "SetThreadActivity";
    case 0x33: return "GetThreadContext3";
    case 0x34: return "WaitForAddress";
    case 0x35: return "SignalToAddress";
    default: return "?";
    }
}

// where in the game the call is: the thunk that brought it ends in a branch back to the instruction after it (tools/relink.py)
static void site_of(const u64 *regs, const char **module, u64 *offset) {
    *module = "?", *offset = 0;
    const u32 *back = (const u32 *)(regs[19] + 4);  // x30 as nx2_svc_entry was called: in the thunk, before its ldp and its b
    if ((*back & 0xFC000000u) != 0x14000000u) return;
    s64 d = (s64)(*back & 0x03FFFFFFu);
    if (d & 0x02000000) d -= 0x04000000;
    nx2_where((u64)back + (u64)(d * 4) - 4, module, offset);
}

// ---------------------------------------------------------------- the messages to the system's services
// Which service a handle is a session of, as far as the game's own messages say: sm:'s GetService names one, and a session a service
// hands back is named after the one that gave it and the command that asked.
// The table is what the port's own answers go by (the file system's sessions get the game's files and saved data from the card), so a
// name must hold for as long as its handle does. It didn't: with 96 places filled in turn, the 97th session a game opened took the
// place of the oldest, and a game, which opens that many, had its file system session forgotten. Its
// next read of the game's files went to the system as it was, which closed the session, and the SDK ended the game (25a0b; log,
// 2026-11-23). Worse could have followed: a request about saved data sent on without its folder acts on the card's root. So: a
// place is freed when its handle is closed, there are many more of them, and the ones the port answers for are never given up.
#define SESSIONS 1024
static struct {
    u32 handle;
    char name[28];
} sessions[SESSIONS];
static u32 session_next, session_lock;
u32 nx2_threads[96];  // the game's threads' handles, in the order they were made (0: not made, or more than these)
static int verbose;        // every call logged (from nnMain on), while the budget lasts
static int budget = 1500;
void nx2_svc_verbose(int on) { verbose = on; }

static void sessions_take(void) {
    while (__atomic_exchange_n(&session_lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
}
static void sessions_give(void) { __atomic_store_n(&session_lock, 0, __ATOMIC_RELEASE); }
static const char *session_name(u32 h) {
    for (u32 k = 0; h && k < SESSIONS; k++)
        if (sessions[k].handle == h) return sessions[k].name;
    return "?";
}
// (the sessions the port answers for: the file system's, the applet's, add-on content's, and sm: itself)
static int session_kept(const char *n) {
    return (n[0] == 'f' && n[1] == 's' && n[2] == 'p') || (n[0] == 'a' && n[1] == 'p' && n[2] == 'p') || (n[0] == 'a' && n[1] == 'o' && n[2] == 'c') ||
           (n[0] == 's' && n[1] == 'm' && n[2] == ':');
}
static void session_forget(u32 h) {  // the handle is being closed: its number may be another session's next
    sessions_take();
    for (u32 k = 0; h && k < SESSIONS; k++)
        if (sessions[k].handle == h) sessions[k].handle = 0, sessions[k].name[0] = 0;
    sessions_give();
}
static void session_set(u32 h, const char *a, const char *b, u32 cmd) {
    char name[28];
    u32 n = 0;
    for (; a && *a && n < 20; a++) name[n++] = *a;
    for (; b && *b && n < 20; b++) name[n++] = *b;
    if (cmd != ~0u) {
        name[n++] = '.';
        if (cmd >= 100) name[n++] = (char)('0' + cmd / 100 % 10);
        if (cmd >= 10) name[n++] = (char)('0' + cmd / 10 % 10);
        name[n++] = (char)('0' + cmd % 10);
    }
    name[n] = 0;
    sessions_take();
    u32 at = SESSIONS;
    for (u32 k = 0; k < SESSIONS && at == SESSIONS; k++)
        if (sessions[k].handle == h) at = k;  // (the same handle again: its new name)
    for (u32 k = 0; k < SESSIONS && at == SESSIONS; k++)
        if (!sessions[k].handle) at = k;
    for (u32 tries = 0; tries < SESSIONS && at == SESSIONS; tries++) {  // full: the oldest that isn't one the port answers for
        const u32 k = session_next++ % SESSIONS;
        if (!session_kept(sessions[k].name)) at = k;
    }
    if (at < SESSIONS) {
        sessions[at].handle = h;
        for (u32 k = 0; k <= n; k++) sessions[at].name[k] = name[k];
    }
    sessions_give();
    if (at == SESSIONS) nx2_log("[ipc] no place left to remember the session %s (handle %x)\n", name, (u64)h);
}
static u8 *tls(void) {
    u8 *p;
    __asm__("mrs %0, tpidrro_el0" : "=r"(p));
    return p;
}
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
// the data of a message: after "SFCI" (a request: the command) or "SFCO" (an answer: the result). Null: none (a control message, a close)
static const u8 *payload(const u8 *m, const char *magic) {
    for (u32 o = 8; o < 0xF0; o += 4)
        if (m[o] == magic[0] && m[o + 1] == magic[1] && m[o + 2] == magic[2] && m[o + 3] == magic[3]) return m + o;
    return NULL;
}
// the first handle an answer carries (a new session), or 0
static u32 answer_handle(const u8 *m) {
    if (!(rd32(m + 4) >> 31)) return 0;
    const u32 d = rd32(m + 8), count = (d >> 1 & 15) + (d >> 5 & 15);
    return count ? rd32(m + 12 + ((d & 1) ? 8 : 0)) : 0;
}

static u32 held[16], held_count;  // the threads made and not to be started (<folder>/hold)
// (the game's own callers at a system call: its frames, from the frame pointer it came in with. The port's stack at a system call
// is its own, so the walk from here, below, never reaches them)
static void game_callers_said(const u64 *r) {
    const u64 *fp = (const u64 *)r[18];
    const char *module = "?";
    u64 off = r[19] & ((1ull << 39) - 1);
    nx2_where(off, &module, &off);
    nx2_log("[from]   the game's return address: %s+%x\n", module, off);
    for (int k = 0; k < 32 && fp && !((u64)fp & 7) && (u64)fp > 0x10000; k++) {
        const u64 from = fp[1] & ((1ull << 39) - 1);
        module = "?", off = from;
        nx2_where(from, &module, &off);
        nx2_log("[from]   the game's caller %u: %s+%x\n", (u64)k, module, off);
        if (fp[0] <= (u64)fp || fp[0] - (u64)fp > 0x400000) break;
        fp = (const u64 *)fp[0];
    }
}
#ifdef __SWITCH2__
// The applet session, on a Switch 2. The system gives a program one registration as an application (appletOE's OpenApplicationProxy),
// and the host library made it at start: the SDK's own was refused (3e880), and after the host's was closed it ended in a fatal error
// of the system's (2128-0007; logs, 2026-11-22). So the SDK shares the host's: its messages to the session it opened go to the
// host's instead, and the two steps that can't be repeated there are answered here: making the session a domain (it is one), and
// the registration (the host's proxy object is handed over). Everything else is the system's own answer.
static u32 applet_sdk, applet_host, applet_proxy;
#define APPLET_ROOT 1u  // the session's own object, as the SDK is told
static u32 applet_receive;  // the message being sent asks for the system's next message to the program
static u32 applet_state, applet_event;  // ICommonStateGetter's object, and the handle of its event (a message is waiting)
static u32 applet_asked;                // the object the request being sent asks command 0 of
static int applet_message = 1;          // the first message is still to be given (below)

// the answer to a request sent on: which object is the state getter, and which handle its event
static void applet_answered(const u8 *m, u32 handle) {
    if (applet_asked == applet_proxy && !applet_state && m[0x10] == 1) {  // GetCommonStateGetter
        applet_state = (u32)m[0x30] | (u32)m[0x31] << 8 | (u32)m[0x32] << 16 | (u32)m[0x33] << 24;
        nx2_log("[applet] the state getter is object %u\n", (u64)applet_state);
    } else if (applet_state && applet_asked == applet_state && handle && !applet_event) {  // GetEventHandle
        applet_event = handle;
    }
    applet_asked = 0;
}

static void applet_named(u32 sdk_handle) {
    if (nx2_ownapplet) return;  // (the SDK's session is its own)
    Service *p = appletGetServiceSession_Proxy();
    if (!serviceIsActive(p) || !p->object_id) return;
    applet_host = p->session, applet_proxy = p->object_id, applet_sdk = sdk_handle;
    nx2_log("[applet] the SDK's applet session (%x) goes to the host library's (%x, proxy object %u)\n", (u64)sdk_handle, (u64)applet_host,
            (u64)applet_proxy);
}
// (for a game whose start ends at the applet session with no word: a message's first bytes, and who sent it, a few frames up)
static void bytes_said(const char *what, const u8 *m) {
    static const char hex[] = "0123456789abcdef";
    char line[3 * 0x50 + 1];
    u32 at = 0;
    for (u32 k = 0; k < 0x50; k++) {
        line[at++] = hex[m[k] >> 4], line[at++] = hex[m[k] & 15];
        if ((k & 3) == 3) line[at++] = ' ';
    }
    line[at] = 0;
    nx2_log("[applet]   %s: %s\n", what, line);
}
static void callers_said(void) {
    const u64 *fp = __builtin_frame_address(0);
    const u64 here = (u64)fp;
    for (int k = 0; k < 24 && fp && !((u64)fp & 7) && (u64)fp >= here && (u64)fp - here < 0x200000; k++) {
        const char *module = "?";
        const u64 from = fp[1] & ((1ull << 39) - 1);  // (a Switch 2 game signs its return addresses: the high bits are the signature)
        u64 off = from;
        nx2_where(from, &module, &off);
        nx2_log("[from]   called from %s+%x\n", module, off);
        if (fp[0] <= (u64)fp) break;
        fp = (const u64 *)fp[0];
    }
}
// 1: answered here. 0: to be sent, to *h
static int applet_ipc(u8 *m, u32 *h, u32 type, u32 cmd) {
    *h = applet_host;
    if (type == 5 || type == 7) {  // a control message
        if (cmd != 0) return 0;
        for (u32 k = 0; k < 0x100; k++) m[k] = 0;  // ConvertCurrentObjectToDomain: done already; the object's id
        m[4] = 8;
        m[0x10] = 'S', m[0x11] = 'F', m[0x12] = 'C', m[0x13] = 'O';
        m[0x20] = APPLET_ROOT;
        return 1;
    }
    u64 buf, size;
    const u8 *d = nx2_ipc_data(m, &buf, &size);
    const u32 kind = d[0], object = (u32)d[4] | (u32)d[5] << 8 | (u32)d[6] << 16 | (u32)d[7] << 24;
    if (kind == 2 && (object == APPLET_ROOT || object == applet_proxy)) {  // closed by the SDK: the host's stay
        nx2_ipc_answer(m, 0, NULL, 0, 0);
        return 1;
    }
    applet_asked = kind == 1 && cmd == 0 ? object : 0;
    applet_receive = kind == 1 && object == applet_state && cmd == 1;  // (ReceiveMessage: the system's messages to the program)
    // The SDK's start waits for the system's message that the program has the screen (15: the focus changed), and the host library
    // took that message at its own start: the SDK waited for good, a black screen (log, 2026-11-22). It is given once more, here.
    if (kind == 1 && object == applet_state && cmd == 1 && applet_message) {  // ICommonStateGetter's ReceiveMessage
        static const u8 focus[4] = {15, 0, 0, 0};
        applet_message = 0;
        nx2_log("[applet] the SDK is given the message that the program has the screen\n");
        nx2_ipc_answer(m, 0, focus, 4, 0);
        return 1;
    }
    // (A Switch 2 game's SDK registers with command 10, the process's number and handle and eight bytes more: sent on to the
    // system it was never answered, and the console stayed black until restarted, 2026-11-24. It is the same registration.)
    if (kind == 1 && object == APPLET_ROOT && (cmd == 0 || cmd == 10)) {  // OpenApplicationProxy
        nx2_log("[applet] the SDK's registration as an application (command %u): given the host library's\n", (u64)cmd);
        nx2_ipc_answer(m, 0, NULL, 0, applet_proxy);
        return 1;
    }
    if (kind == 1 && object == APPLET_ROOT) {  // nothing else of the session's own object is sent to the system
        nx2_log("[applet] command %u of the applet session itself isn't one this port knows: refused\n", (u64)cmd);
        nx2_ipc_answer(m, 0x3E880, NULL, 0, 0);
        return 1;
    }
    return 0;
}
#endif

// SendSyncRequest: made here, so the answer is seen too
static void send_sync(u64 *r, const char *module, u64 off, int log) {
    u8 *m = tls();
    u32 h = (u32)r[0];
    const u8 *q = payload(m, "SFCI");
    const u32 type = rd32(m) & 0xFFFF;
    u32 cmd = q ? rd32(q + 8) : ~0u;
    char text[9] = {0};
    for (u32 k = 0; q && k < 8; k++) text[k] = (q[16 + k] >= 0x20 && q[16 + k] < 0x7F) ? (char)q[16 + k] : 0;
    const char *who = session_name(h);
    const int is_sm = who[0] == 's' && who[1] == 'm' && who[2] == ':';
    // A later SDK speaks to sm: in the system's newer, shorter form (TIPC: the command is in the message's type, 16 and up, and the
    // data follows the header at once, with no "SFCI"). Its request for a service is type 17, the name at the eighth byte. Not seen
    // as one, every service a game (SDK 16.2) opened was taken for sm: itself, the file system
    // service among them: its files and saved data were then the system's, of the program this was started over (log, 2026-11-22).
    const int tipc = is_sm && !q && type == 17;
#ifdef NX2_GAME_MB
    // (A Switch 2 SDK's messages in the shorter form to anything but sm:, its graphics driver's by the hundred for each picture:
    // none is this port's to answer or to learn a session from. Sent as they are.)
    if (!is_sm && !q && type >= 16 && !log) {
        r[0] = svcSendSyncRequest(h);
        return;
    }
#endif
    if (tipc) {
        cmd = 1;
        for (u32 k = 0; k < 8; k++) text[k] = (m[8 + k] >= 0x20 && m[8 + k] < 0x7F) ? (char)m[8 + k] : 0;
    }
#ifdef __SWITCH2__
    if (q && (type == 4 || type == 6) && who[0] == 'l' && who[1] == 'd' && who[2] == 'r' && who[3] == ':' && who[4] == 'r' && who[5] == 'o' && !who[6] &&
        nx2_ro_ipc(m, q, cmd, module, off)) {  // a module the game loads as it runs (ro.c)
        r[0] = nx2_ro_kernel;
        return;
    }
#endif
    if (who[0] == 'f' && who[1] == 's' && who[2] == 'p' && nx2_rom_ipc(m)) {  // the game's files, from the card (rom.c)
        r[0] = 0;
        return;
    }
    if (who[0] == 'a' && who[1] == 'o' && who[2] == 'c' && nx2_aoc_ipc(m)) {  // its add-on content, likewise
        r[0] = 0;
        return;
    }
    // Before it aborts, the SDK tells the system why the application ends (the applet session's SetTerminateResult, command 22). The
    // application, to the system, is the installed game this was started over: when the reason was one of the file system's "data is
    // corrupted" results (217202, from a RomFS image this project's own tool had made wrongly), the system marked that game as
    // corrupted, twice, two games (2026-11-22). The report is the port's to keep: it is answered here and never sent.
    if (who[0] == 'a' && who[1] == 'p' && who[2] == 'p' && who[3] == 'l' && who[4] == 'e' && who[5] == 't' && q && cmd == 22 &&
        (type == 4 || type == 6)) {
        u64 b, bs;
        const u8 *d = nx2_ipc_data(m, &b, &bs);
        const u32 reason = d + 36 <= m + 0x100 ? rd32(d + 32) : 0;
        nx2_log("[applet] the SDK reports why the game ends (result %x): kept from the system\n", (u64)reason);
        nx2_ipc_answer(m, 0, NULL, 0, 0);
        r[0] = 0;
        return;
    }
    // How long the game has been played, asked of the system by application id (the applet session's QueryApplicationPlayStatistics,
    // commands 110 and 111). The system answers only an application allowed to ask about that id, and the application is the one
    // this was started over: under one installed game a game was answered, under another it was refused
    // (3e880) and the SDK ended it (log, 2026-11-23). So the answer is the port's: none played, whatever it was started over.
    // (Only a request with a buffer to be filled: another of the session's objects has a command 110 that sets something.)
    if (who[0] == 'a' && who[1] == 'p' && who[2] == 'p' && who[3] == 'l' && who[4] == 'e' && who[5] == 't' && q && (cmd == 110 || cmd == 111) &&
        (type == 4 || type == 6) && (rd32(m) >> 24 & 15)) {
        static const u8 none[4];
        nx2_log("[applet] the game asks how long it has been played (command %u): told of none\n", (u64)cmd);
        nx2_ipc_answer(m, 0, none, 4, 0);
        r[0] = 0;
        return;
    }
    char save_paths[2][0x301];
    u32 save_handle = 0;
    const int save = who[0] == 'f' && who[1] == 's' && who[2] == 'p' ? nx2_save_before(m, save_paths, &save_handle) : 0;  // saved data, on the card (rom.c)
    if (save == 5) h = save_handle;  // (through the host's own session of the file system service)
    if (save == 3) {
        r[0] = 0;
        return;
    }
#ifdef __SWITCH2__
    // The SDK asks sm: for audout:u, which a Switch 2 refuses this program: it is given a new session of the audio output manager
    // main.c opened through aud:u instead (the same object). The request sent is "clone this session", to that manager; its answer
    // has the shape of sm:'s (a result, and the session's handle).
    // (The same for audren:u, the audio renderer's manager, which most games use: a game, a game.)
    const int wants_out = text[0] == 'a' && text[1] == 'u' && text[2] == 'd' && text[3] == 'o' && text[4] == 'u' && text[5] == 't' &&
                          text[6] == ':' && text[7] == 'u';
    const int wants_ren = text[0] == 'a' && text[1] == 'u' && text[2] == 'd' && text[3] == 'r' && text[4] == 'e' && text[5] == 'n' &&
                          text[6] == ':' && text[7] == 'u';
    if (is_sm && cmd == 1 && ((wants_out && nx2_audio_master) || (wants_ren && nx2_audren_master))) {
        for (u32 k = 0; k < 0x40; k++) m[k] = 0;
        m[0] = 5, m[4] = 8;  // a control message, 8 words of data
        m[0x10] = 'S', m[0x11] = 'F', m[0x12] = 'C', m[0x13] = 'I';
        m[0x18] = 2;  // CloneCurrentObject
        h = wants_out ? nx2_audio_master : nx2_audren_master;
    }
    static u32 applet_said;
    const int applet_watch = applet_sdk && h == applet_sdk && applet_said < 16;
    if (applet_watch) {
        applet_said++;
        nx2_log("[applet] message %u to the applet session: type %u, command %d\n", (u64)applet_said, (u64)type, (s64)(s32)cmd);
        bytes_said("asked", m);
        callers_said();
    }
    if (applet_sdk && h == applet_sdk && applet_ipc(m, &h, type, cmd)) {
        if (log) nx2_log("[ipc] %s type %u command %d: answered here, from %s+%x\n", who, (u64)type, (s64)(s32)cmd, module, off);
        if (applet_watch) bytes_said("answered here", m);
        r[0] = 0;
        return;
    }
#endif
    const u32 rc = svcSendSyncRequest(h);
#ifdef __SWITCH2__
    if (applet_watch) bytes_said("the system answered", m);
    // (the system's message that the program is to close, 4: the user closed the game. A run that ends after it is not one that
    // ended badly; log.c)
    if (applet_sdk && h == applet_sdk && applet_receive && !rc) {
        const u8 *o = payload(m, "SFCO");
        if (o && !rd32(o + 8) && rd32(o + 16) == 4) {
            nx2_log("[applet] the system tells the game to close\n");
            nx2_closing();
        }
    }
#endif
    if ((save == 1 || save == 5) && !rc) nx2_save_after(m, save);
#ifdef __SWITCH2__
    // (the answer to "clone this session", for an SDK that asked in the newer form: the result where that form has it, after the handle)
    if (tipc && h != (u32)r[0] && !rc) {
        const u8 *o = payload(m, "SFCO");
        const u32 res = o ? rd32(o + 8) : 0xE401;
        m[0x10] = (u8)res, m[0x11] = (u8)(res >> 8), m[0x12] = (u8)(res >> 16), m[0x13] = (u8)(res >> 24);
        m[4] = 1, m[5] = 0;  // one word of data (the handle's descriptor is kept)
    }
#endif
    const u8 *a = payload(m, "SFCO");
    const u32 result = a ? rd32(a + 8) : 0, got = rc ? 0 : answer_handle(m);
#ifdef __SWITCH2__
    if (applet_sdk && (u32)r[0] == applet_sdk && !rc && !result) applet_answered(m, got);
#endif
    if (got && !result) {
        if (is_sm && cmd == 1) {
            session_set(got, text, NULL, ~0u);
#ifdef __SWITCH2__
            if (text[0] == 'a' && text[1] == 'p' && text[2] == 'p' && text[3] == 'l' && text[4] == 'e' && text[5] == 't') applet_named(got);
#endif
        }
        else session_set(got, who, NULL, cmd);
    }
    // (a request that failed is always said, a few hundred times at most: it is what a game is most often ended for, and the log's
    // budget of ordinary lines is long spent by then)
    static int failures = 300;
    const int failed = (rc && !(rc == 0xF601 && type == 2)) || result;
    if (log || (failed && __atomic_fetch_sub(&failures, 1, __ATOMIC_RELAXED) > 0))
        nx2_log("[ipc] %s (handle %x) type %u command %d%s%s: kernel %x, result %x%s%s, from %s+%x%s", who, (u64)h, (u64)type, (s64)(s32)cmd,
                is_sm && cmd == 1 ? " " : "", is_sm && cmd == 1 ? text : "", (u64)rc, (u64)result, got ? ", a session: " : "",
                got ? session_name(got) : "", module, off, "\n");
    r[0] = rc;
}

void nx2_vsync_tick(void);  // gl2.c (a Switch 2's)
int nx2_svc(u64 *r, u32 n) {
#ifdef __SWITCH2__
    if (n == 0x0b) nx2_vsync_tick();  // (SleepThread)
#endif
#ifdef NX2_GAME_MB
    // (How much of its stack a thread has left when it makes a system call: the call costs it nothing on its console, and here the
    // registers are kept on its stack, 192 bytes below where it was. A thread of 4 KB may not have them. Each new least is said.)
    if (nx2_stackwatch) {
        static u64 least = ~0ull;
        MemoryInfo mi;
        u32 pi;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)r))) {
            const u64 left = (u64)r - mi.addr, was = __atomic_load_n(&least, __ATOMIC_RELAXED);
            if (left < was && left < 0x1000) {
                __atomic_store_n(&least, left, __ATOMIC_RELAXED);
                const char *module;
                u64 off;
                site_of(r, &module, &off);
                nx2_log("[stack] system call %x from %s+%x: %u bytes of the thread's stack left below the port's 192 (the stack's memory: %x bytes at %x, kind %u)\n",
                        (u64)n, module, off, left, mi.size, mi.addr, (u64)mi.type);
            }
        }
    }
#endif
    const u32 seen = n < 0xC0 ? __atomic_fetch_add(&counts[n], 1, __ATOMIC_RELAXED) : 0;
#ifdef NX2_GAME_MB
    // (The first use of each system call, always said: one the kernel doesn't allow this process, which is an installed Switch
    // game's, ends the program at once and with no word, as a try of this port's own showed; 2026-11-24.)
    if (!seen) {
        const char *module;
        u64 off;
        site_of(r, &module, &off);
        nx2_log("[svc] the first use of system call %x %s (%x, %x, %x, %x), from %s+%x\n", (u64)n, name_of(n), r[0], r[1], r[2], r[3], module, off);
    }
#endif
    // (from nnMain on, every call but the locks' and the clock's, which come by the thousand)
    const int quiet = n == 0x1A || n == 0x1B || n == 0x1C || n == 0x1D || n == 0x1E || n == 0x10 || n == 0x34 || n == 0x35;
    const int more = verbose && nx2_debug && !quiet && __atomic_fetch_sub(&budget, 1, __ATOMIC_RELAXED) > 0;
#ifdef __SWITCH2__
    {  // (who asks what memory an address inside the program is in, or what is at the program's start: a few times)
        static u32 asked;
        if (n == 0x06 && verbose && ((r[2] & 0xFFF) || r[2] == 0x8000000) && r[2] < 0x10000000 && __atomic_fetch_add(&asked, 1, __ATOMIC_RELAXED) < 6) {
            nx2_log("[svc] QueryMemory of %x, asked by:\n", r[2]);
            callers_said();
        }
    }
#endif
#ifdef __SWITCH2__
    if (n == 0x80) {  // (a call a Switch 2's SDK makes on memory its graphics driver uses: made here, so that what the kernel says is seen)
        typedef struct {
            u64 x0, x1;
        } Two;
        extern const u32 nx2_svc_stubs[];  // (svc_entry.s: "svc n; ret", 8 bytes each)
        const u64 a = r[0], b = r[1], c = r[2], d = r[3];
        const Two out = ((Two (*)(u64, u64, u64, u64, u64, u64, u64, u64))(nx2_svc_stubs + 2 * 0x80))(a, b, c, d, r[4], r[5], r[6], r[7]);
        r[0] = out.x0, r[1] = out.x1;
        static u32 said;
        if (out.x0 || __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 12)
            nx2_log("[svc] 80 (%x, %x, %x, %x): the kernel says %x (then %x)\n", a, b, c, d, out.x0, out.x1);
        return 1;
    }
#endif
    if (n == 0x21) {
        const char *module;
        u64 off;
        site_of(r, &module, &off);
        send_sync(r, module, off, seen < 3 || more || nx2_ipclog);
        return 1;
    }
    if (n == 0x1F) {  // ConnectToNamedPort(out handle, name): sm: is where every service is asked for
        const char *name = (const char *)r[1];
        Handle h = 0;
        u32 rc = svcConnectToNamedPort(&h, name);
        nx2_log("[svc] 1f ConnectToNamedPort %s: %x, handle %x\n", name, (u64)rc, (u64)(rc ? 0 : h));
        // Refused on a Switch (10801: the process's limit of sessions reached; log, 2026-11-22), where the host library has one to
        // sm: already: the SDK is given that one.
        if (rc && name[0] == 's' && name[1] == 'm' && name[2] == ':' && !name[3] && serviceIsActive(smGetServiceSession())) {
            h = smGetServiceSession()->session, rc = 0;
            nx2_log("[svc] ... the SDK gets the host library's session to sm: (handle %x)\n", (u64)h);
        }
        if (!rc) session_set(h, name, NULL, ~0u);
        r[0] = rc, r[1] = h;
        return 1;
    }
    if (seen < 3 || more) {
        const char *module;
        u64 off;
        site_of(r, &module, &off);
        nx2_log("[svc] %2x %s (%x, %x, %x, %x) from %s+%x%s\n", (u64)n, name_of(n), r[0], r[1], r[2], r[3], module, off,
                seen == 2 && !verbose ? " (its later uses aren't logged)" : "");
    }
    if (n == 0x16) session_forget((u32)r[0]);  // CloseHandle
    switch (n) {
#ifdef __SWITCH2__
    case 0x18:  // WaitSynchronization(out index, handles, count, timeout): the applet's event, with the message above still to give
        if (applet_message && applet_event && r[2] == 1 && *(const u32 *)r[1] == applet_event) {
            r[0] = 0, r[1] = 0;
            return 1;
        }
        return 0;
#endif
    case 0x29: {  // GetInfo(out, type, handle, sub)
        // Memory in use (7, and 22 without the system's part): the host library has taken nearly all of it for its own heap, so a
        // game that sizes its heap by what is free (total - used) got 2 MB and its first large allocation failed (A game's nninitStartup; log, 2026-11-22). What's free, as the game is told, is the block set aside for its heap.
        const u32 type = (u32)r[1];
#ifdef NX2_GAME_MB
        // (A Switch 2 game's own: a game ends itself at its start unless the memory in use is less than its "Program"
        // heap, 250 MB in its HeapDef file, which is the program alone on its console; main+5c9f58, log, 2026-11-24. Here the host's
        // share is in use as well. So the game is told of a console with its heap and a program of 128 MB, and nothing else.)
        if ((type == 6 || type == 7 || type == 21 || type == 22) && heap_max) {
            r[0] = 0, r[1] = (128ull << 20) + (type == 6 || type == 21 ? heap_max : 0);
            return 1;
        }
#endif
        if ((type == 7 || type == 22) && heap_max) {
            u64 total = 0;
            if (R_SUCCEEDED(svcGetInfo(&total, type == 7 ? 6 : 21, (Handle)r[2], 0)) && total > heap_max) {
                r[0] = 0, r[1] = total - heap_max;
                return 1;
            }
        }
        return 0;
    }
    case 0x04: {  // MapMemory(to, from, size): done here, so that a refusal is seen (a thread's stack is mapped so by the SDK)
        const u32 rc = svcMapMemory((void *)r[0], (void *)r[1], r[2]);
        static u32 maps;
        const u32 count = __atomic_add_fetch(&maps, 1, __ATOMIC_RELAXED);
        if (rc || (nx2_debug && r[2] > 0x100000)) nx2_log("[svc] MapMemory %u: %x bytes of %p to %p: %x%s\n", (u64)count, r[2], r[1], r[0], (u64)rc, rc ? " (REFUSED)" : "");
        r[0] = rc;
        return 1;
    }
    case 0x08: {  // CreateThread(out handle, entry, argument, stack top, priority, core): done here, so that what the kernel says is seen
        static u32 made;
        if (nx2_debug) {
            const char *em = "?";
            u64 eo = r[1] & ((1ull << 39) - 1);
            nx2_where(eo, &em, &eo);
            nx2_log("[svc] a thread is asked for: its function %s+%x, given %p, stack top %p" "\n", em, eo, r[2], r[3]);
        }
        Handle h = 0;
        const u32 rc = svcCreateThread(&h, (ThreadFunc)r[1], (void *)r[2], (void *)r[3], (int)r[4], (int)r[5]);
        const u32 count = __atomic_add_fetch(&made, 1, __ATOMIC_RELAXED);
        if (!rc && count <= sizeof nx2_threads / sizeof *nx2_threads) nx2_threads[count - 1] = h;  // (for the watcher, loader.c)
#ifdef __SWITCH2__
        // (A thread the game asks no core for, "the default one": the system here has that be core 0 alone for every such thread,
        // and a game whose threads were made to share three cores had them all take turns on one. A game makes 46
        // threads so; measured, 2026-11-24. Each is given the first three cores to move among, one of them to prefer, in turn.
        // (No file is looked for here to turn it off: this runs as the game, where the C library isn't to be used.)
        if (!rc && (s32)r[5] == -2) {
            static int asked, spread;
            if (!asked) {
                asked = 1;
                u64 allowed = 0;
                svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
                spread = (allowed & 7) == 7 && !nx2_onecore;
                nx2_log("[svc] the game's threads that ask for no core: %s (the system allows cores %x)\n", spread ? "given cores 0 to 2 to move among" : "left on the default core", allowed);
            }
            if (spread && !nx2_pin) svcSetThreadCoreMask(h, (s32)(count % 3), 7);
        }
        // (<folder>/pin: a thread never changes core. A try for a Switch 2 game's code, which signs its return addresses with a key
        // of the processor's: were the key not the same on every core for this process, a thread moved between a function's start
        // and its return would return to nowhere.)
        if (!rc && nx2_pin) {
            const s32 core = (s32)r[5] >= 0 ? (s32)r[5] : (s32)(count % 3);
            svcSetThreadCoreMask(h, core, 1u << core);
        }
#endif
#ifdef NX2_GAME_MB
        if (!rc) {
            s32 now = -9;
            u64 mask_now = 0;
            svcGetThreadCoreMask(&now, &mask_now, h);
            nx2_log("[svc] thread %u of the game's (handle %x): priority %d, core %d asked; it has core %d, cores %x\n", (u64)count, (u64)h, (s64)(s32)r[4],
                    (s64)(s32)r[5], (s64)now, mask_now);
        }
#endif
        if (rc || nx2_debug) nx2_log("[svc] thread %u of the game's: priority %d, core %d: %x%s\n", (u64)count, (s64)(s32)r[4], (s64)(s32)r[5], (u64)rc, rc ? " (REFUSED)" : "");
        if (!rc && nx2_hold_from && (int)count >= nx2_hold_from && (int)count <= nx2_hold_to && held_count < 16) held[held_count++] = h;
        r[0] = rc, r[1] = h;
        return 1;
    }
    case 0x09:  // StartThread(handle): not done for a thread that is held (<folder>/hold)
        for (u32 k = 0; k < held_count; k++)
            if (held[k] == (u32)r[0]) {
                nx2_log("[svc] thread %x is held: not started (<folder>/hold)" "\n", r[0]);
                r[0] = 0;
                return 1;
            }
        return 0;
    case 0x2C:
    case 0x2D: {  // MapPhysicalMemory, UnmapPhysicalMemory(address, size): done here, so that what the kernel says is seen
        const u32 rc = n == 0x2C ? svcMapPhysicalMemory((void *)r[0], r[1]) : svcUnmapPhysicalMemory((void *)r[0], r[1]);
        static u64 mapped;
        static u32 said;
        const u64 now = n == 0x2C ? __atomic_add_fetch(&mapped, rc ? 0 : r[1], __ATOMIC_RELAXED) : __atomic_sub_fetch(&mapped, rc ? 0 : r[1], __ATOMIC_RELAXED);
        const u32 k = __atomic_add_fetch(&said, 1, __ATOMIC_RELAXED);
        if (rc || k <= 8 || (k & (k - 1)) == 0) {
            u64 total = 0, used = 0;
            svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
            svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            nx2_log("[svc] %s of %x bytes at %x: %x%s (call %u; %u KB mapped so; the process has %u KB free)\n", n == 0x2C ? "MapPhysicalMemory" : "UnmapPhysicalMemory",
                    r[1], r[0], (u64)rc, rc ? " (REFUSED)" : "", (u64)k, now >> 10, (total - used) >> 10);
        }
        r[0] = rc;
        return 1;
    }
    case 0x45: {  // CreateEvent(out writable handle, out readable handle): done here, so that what the kernel says is seen
        Handle w = 0, rd = 0;
        const u32 rc = svcCreateEvent(&w, &rd);
        static u32 made;
        const u32 k = __atomic_add_fetch(&made, 1, __ATOMIC_RELAXED);
        if (rc || (k & (k - 1)) == 0) nx2_log("[svc] CreateEvent %u: %x%s (handles %x, %x)\n", (u64)k, (u64)rc, rc ? " (REFUSED)" : "", (u64)w, (u64)rd);
        r[0] = rc, r[1] = w, r[2] = rd;
        return 1;
    }
    case 0x15: {  // CreateTransferMemory(out handle, address, size, access): done here, so that what the kernel says is seen
        Handle h = 0;
        const u32 rc = svcCreateTransferMemory(&h, (void *)r[1], r[2], (u32)r[3]);
        static u32 said;
        if (rc || __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 16)
            nx2_log("[svc] CreateTransferMemory of %x bytes at %x, access %u: %x%s\n", r[2], r[1], r[3], (u64)rc, rc ? " (REFUSED)" : "");
        r[0] = rc, r[1] = h;
        return 1;
    }
    case 0x13: {  // MapSharedMemory(handle, address, size, access)
        const u32 rc = svcMapSharedMemory((Handle)r[0], (void *)r[1], r[2], (u32)r[3]);
        static u32 said;
        if (rc || __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED) < 16)
            nx2_log("[svc] MapSharedMemory of %x bytes at %x, access %u: %x%s\n", r[2], r[1], r[3], (u64)rc, rc ? " (REFUSED)" : "");
        r[0] = rc;
        return 1;
    }
#ifdef __SWITCH2__
    case 0x0F: {  // SetThreadCoreMask(thread, core, mask)
        if (!nx2_pin) {
#ifdef NX2_GAME_MB
            // (a Switch 2 game's own: done here, so that what was asked and what the kernel said are seen, every time)
            const u32 h = (u32)r[0], rc = svcSetThreadCoreMask((Handle)r[0], (s32)r[1], (u32)r[2]);
            s32 now = -9;
            u64 mask_now = 0;
            svcGetThreadCoreMask(&now, &mask_now, (Handle)h);
            nx2_log("[svc] SetThreadCoreMask of thread %x: core %d, cores %x asked: %x%s; it has core %d, cores %x now\n", (u64)h, (s64)(s32)r[1], r[2],
                    (u64)rc, rc ? " (REFUSED)" : "", (s64)now, mask_now);
            r[0] = rc;
            return 1;
#else
            return 0;
#endif
        }
        const u64 mask = r[2] & 0x3F;
        s32 core = (s32)r[1];
        if (!mask) return 0;  // (no change of the mask asked: the kernel's own)
        if (core < 0 || !(mask >> core & 1)) core = __builtin_ctzll(mask);
        r[0] = svcSetThreadCoreMask((Handle)r[0], core, 1u << core);
        return 1;
    }
#endif
    case 0x01: {  // SetHeapSize(out address, size): the host's heap is this program's; the game's is a block of it
        const u64 size = r[1];
        if (live_base) {  // (a Switch 2 game's: the kernel's own heap, made larger after the host's part)
            void *at = NULL;
            const u32 rc = size > heap_max ? 0xD201 : svcSetHeapSize(&at, live_host + size);
            nx2_log("[svc] SetHeapSize %x (%u MB): the kernel's heap made the host's %u MB and that: %x%s\n", size, size >> 20, live_host >> 20, (u64)rc,
                    size > heap_max ? " (more than the game was told of)" : rc ? " (REFUSED)" : "");
            if (!rc) heap_now = size;
            r[0] = rc;
            r[1] = live_base + live_host;
            return 1;
        }
        if (!heap_base || size > heap_max) {
            nx2_log("[svc] SetHeapSize %x: more than the %x set aside\n", size, heap_max);
            r[0] = 0xD201;  // out of memory (kernel)
            return 1;
        }
        heap_now = size;
        r[0] = 0;
        r[1] = (u64)heap_base;
        return 1;
    }
    case 0x07:  // ExitProcess
        nx2_log("[svc] the game exits\n");
        for (u32 k = 0; k < 0xC0; k++)
            if (counts[k]) nx2_log("[svc] %2x %s: %u calls\n", (u64)k, name_of(k), (u64)counts[k]);
        nx2_exit();
    case 0x26: {  // Break(reason, address, size)
        const char *module;
        u64 off;
        site_of(r, &module, &off);
        nx2_log("[svc] Break: reason %x, %x, %x, from %s+%x\n", r[0], r[1], r[2], module, off);
        game_callers_said(r);
        if (r[1] && r[2] == 4) nx2_log("[svc] Break: the result given is %x\n", (u64) * (const u32 *)r[1]);
        if (r[0] & 0x80000000u) {  // a notification only: the game goes on
            r[0] = 0;
            return 1;
        }
        nx2_crash("the game's SDK ended the program itself (an abort: the lines before \"Break\" at the log's end say what it had just asked for)");
        nx2_exit();
    }
    case 0x27: {  // OutputDebugString(text, size)
        char text[200];
        const u64 len = r[1] < sizeof text - 1 ? r[1] : sizeof text - 1;
        for (u64 k = 0; k < len; k++) text[k] = ((const char *)r[0])[k];
        text[len] = 0;
        nx2_log("[game] %s%s", text, len && text[len - 1] == '\n' ? "" : "\n");
        r[0] = 0;
        return 1;
    }
    default: return 0;
    }
}
