/*
 * xinput1_3.dll proxy for Halo: The Master Chief Collection (MCC-Win64-Shipping.exe, x64).
 *
 * "Dual input": the gamepad stays the game's active device (gamepad glyphs) while
 * mouse movement (Steam Input generates it from gyro) is turned into right stick
 * deflection. The real right stick (Steam right trackpad) above a deadzone always wins.
 *
 * How it works
 *  - The exe imports XINPUT1_3.dll by ordinal only (#2 XInputGetState, #3 XInputSetState),
 *    so this DLL exports those ordinals and forwards to the real system DLL.
 *  - Steam Input detours XInputGetState, so code inside our export may never run. We
 *    patch the exe's IAT slot for XINPUT1_3 #2 to HookGetState, which calls whatever the
 *    slot held before (Steam's pointer) and mixes the mouse into the result.
 *  - The mouse comes from Raw Input: the exe's user32!GetRawInputData IAT slot is patched
 *    to HookGetRawInputData, which accumulates the deltas and neutralises the packet.
 *  - The main window is subclassed to swallow WM_MOUSEMOVE during gameplay.
 *  - GAMEPLAY / MENU is decided by a flag byte in the exe (build-specific) or by cursor
 *    visibility. In MENU everything passes through untouched.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

/* ---- minimal XInput definitions (no xinput.h so nothing clashes with our exports) ---- */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGAMEPAD;
typedef struct { DWORD dwPacketNumber; XGAMEPAD Gamepad; } XSTATE;

typedef DWORD (WINAPI *PFN_GetState)(DWORD, XSTATE *);
typedef DWORD (WINAPI *PFN_SetState)(DWORD, void *);
typedef DWORD (WINAPI *PFN_GetCaps)(DWORD, DWORD, void *);
typedef void  (WINAPI *PFN_Enable)(BOOL);
typedef DWORD (WINAPI *PFN_Guids)(DWORD, LPWSTR, UINT *, LPWSTR, UINT *);
typedef DWORD (WINAPI *PFN_Battery)(DWORD, BYTE, void *);
typedef DWORD (WINAPI *PFN_Keystroke)(DWORD, DWORD, void *);
typedef UINT  (WINAPI *PFN_GetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);

#define EXPECTED_EXE_SIZE 67179480ULL
#define XINPUT_GETSTATE_SLOT_RVA 0x286c160ULL   /* VA 0x14286c160 - 0x140000000, for logging only */

/* ---- globals ---- */
static HMODULE g_self, g_real;
static char    g_realPath[MAX_PATH];
static char    g_exePath[MAX_PATH];
static char    g_ini[MAX_PATH], g_logPath[MAX_PATH];
static BYTE   *g_exe;
static DWORD   g_exeImageSize;
static ULONGLONG g_exeFileSize;
static LARGE_INTEGER g_freq;

static PFN_GetState  p_GetState;
static PFN_SetState  p_SetState;
static PFN_GetCaps   p_GetCaps;
static PFN_Enable    p_Enable;
static PFN_Guids     p_Guids;
static PFN_Battery   p_Battery;
static PFN_Keystroke p_Keystroke;

/* settings */
static volatile LONG g_enabled = 1;
static int   g_padIndex   = 0;
static float g_sens       = 0.3f;    /* 1.0 = ~1000 counts/s gives full stick */
static float g_curve      = 1.0f;
static float g_windowMs   = 30.0f;
static float g_antiDz     = 0.30f;
static float g_override   = 0.27f;
static int   g_invertY    = 0;
static int   g_invertX    = 0;
static int   g_rawSuppress = 1;
static int   g_blockMove  = 1;
static int   g_menuDetect = 0;
static DWORD g_flagRva    = 0x4001b63;
static int   g_modeSel    = 2;       /* 1 = mouse->right stick, 2 = native mouse injection via GetInput hook */
static float g_gyroSens   = 1.0f;
static int   g_trackpadOv = 1;
static int   g_logOn      = 1;

/* candidate flag bytes (RVAs), logged on transitions/status */
static const DWORD k_candRva[4] = { 0x4001b63, 0x4001b62, 0x4001b66, 0x3caf8d8 };

/* ---------------------------------------------------------------- logging */
static CRITICAL_SECTION g_logCs;
static FILE *g_logFile;

static void Log(const char *fmt, ...)
{
    va_list ap;
    SYSTEMTIME st;
    if (!g_logOn || !g_logFile) return;
    EnterCriticalSection(&g_logCs);
    GetLocalTime(&st);
    fprintf(g_logFile, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
    LeaveCriticalSection(&g_logCs);
}

static LONGLONG Now(void)
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
}

/* ---------------------------------------------------------------- settings */
static const char k_defaultIni[] =
    "; xinput_mouse.ini - dual input proxy for Halo MCC (mouse/gyro -> right stick)\r\n"
    "[Mouse]\r\n"
    "; Mode: 2 = native mouse injection (gyro as real mouse look, linear, no stick curve; hooks the host input builder),\r\n"
    ";       1 = old behaviour: mouse converted to right stick deflection (uses Sensitivity/Curve/SmoothingMs/AntiDeadzone)\r\n"
    "Mode=2\r\n"
    "; Mode 2: multiplier on top of the game's own mouse sensitivity\r\n"
    "GyroSensitivity=1.0\r\n"
    "; Mode 2: 1 = real right stick / trackpad above StickOverrideDeadzone discards gyro\r\n"
    "TrackpadOverride=1\r\n"
    "; 1 = mouse handling active, 0 = pure pass-through. F10 toggles at runtime.\r\n"
    "Enabled=1\r\n"
    "; XInput user index that receives the mouse (0 = first pad)\r\n"
    "PadIndex=0\r\n"
    "; mouse speed -> stick. 1.0 = ~1000 counts/s gives full deflection\r\n"
    "Sensitivity=0.3\r\n"
    "; response exponent: <1 boosts small moves, >1 damps them\r\n"
    "Curve=1.0\r\n"
    "; mouse speed is averaged over this window (ms, 5..200)\r\n"
    "SmoothingMs=30\r\n"
    "; added to any non-zero mouse deflection to jump over the game's radial stick deadzone (0..0.9)\r\n"
    "AntiDeadzone=0.30\r\n"
    "; real right stick (trackpad) above this fraction of full range overrides the mouse entirely\r\n"
    "StickOverrideDeadzone=0.27\r\n"
    "InvertY=0\r\n"
    "InvertX=0\r\n"
    "; Raw mouse packets: 0 = leave to the game, 1 = zero the deltas (buttons/wheel kept),\r\n"
    "; 2 = drop pure-movement packets completely (GetRawInputData returns -1), else zero deltas\r\n"
    "RawSuppress=1\r\n"
    "; swallow WM_MOUSEMOVE of the game window during gameplay\r\n"
    "BlockMouseMove=1\r\n"
    "; gameplay/menu detection: 1 = flag byte in the exe (FlagRva), 2 = cursor hidden heuristic, 0 = always gameplay\r\n"
    "MenuDetect=0\r\n"
    "; RVA (hex) of the 'mouse locked' byte; nonzero = gameplay. Only valid for the exe build it was found in\r\n"
    "FlagRva=0x4001b63\r\n"
    "; write mixed_input.log next to the DLL\r\n"
    "Log=1\r\n";

static float GetF(const char *key, const char *def)
{
    char buf[64];
    GetPrivateProfileStringA("Mouse", key, def, buf, sizeof buf, g_ini);
    return (float)atof(buf);
}

static void LoadSettings(void)
{
    char buf[64];
    DWORD attr = GetFileAttributesA(g_ini);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(g_ini, "wb");
        if (f) { fwrite(k_defaultIni, 1, sizeof k_defaultIni - 1, f); fclose(f); }
    }
    g_enabled     = GetPrivateProfileIntA("Mouse", "Enabled", 1, g_ini) != 0;
    g_padIndex    = GetPrivateProfileIntA("Mouse", "PadIndex", 0, g_ini);
    g_sens        = GetF("Sensitivity", "0.3");
    g_curve       = GetF("Curve", "1.0");
    g_windowMs    = GetF("SmoothingMs", "30");
    g_antiDz      = GetF("AntiDeadzone", "0.30");
    g_override    = GetF("StickOverrideDeadzone", "0.27");
    g_invertY     = GetPrivateProfileIntA("Mouse", "InvertY", 0, g_ini);
    g_invertX     = GetPrivateProfileIntA("Mouse", "InvertX", 0, g_ini);
    g_rawSuppress = GetPrivateProfileIntA("Mouse", "RawSuppress", 1, g_ini);
    g_blockMove   = GetPrivateProfileIntA("Mouse", "BlockMouseMove", 1, g_ini);
    g_menuDetect  = GetPrivateProfileIntA("Mouse", "MenuDetect", 0, g_ini);
    g_modeSel     = GetPrivateProfileIntA("Mouse", "Mode", 2, g_ini) == 1 ? 1 : 2;
    g_gyroSens    = GetF("GyroSensitivity", "1.0");
    g_trackpadOv  = GetPrivateProfileIntA("Mouse", "TrackpadOverride", 1, g_ini) != 0;
    g_logOn       = GetPrivateProfileIntA("Mouse", "Log", 1, g_ini);
    GetPrivateProfileStringA("Mouse", "FlagRva", "0x4001b63", buf, sizeof buf, g_ini);
    g_flagRva     = (DWORD)strtoul(buf, NULL, 16);
    if (g_curve <= 0.05f) g_curve = 1.0f;
    if (g_windowMs < 5.0f) g_windowMs = 5.0f;
    if (g_windowMs > 200.0f) g_windowMs = 200.0f;
    if (g_antiDz < 0.0f) g_antiDz = 0.0f;
    if (g_antiDz > 0.9f) g_antiDz = 0.9f;
    if (g_padIndex < 0 || g_padIndex > 3) g_padIndex = 0;
}

/* ---------------------------------------------------------------- mode detection */
#define MODE_MENU 0
#define MODE_GAMEPLAY 1
static volatile LONG g_mode = -1;
static volatile LONGLONG g_modeTs;
static int g_effMenuDetect;       /* effective method after validation */

/* returns byte value or -1 if the address is outside the image / unreadable */
static int ReadExeByte(DWORD rva)
{
    int v = -1;
    if (!g_exe || rva >= g_exeImageSize) return -1;
    __try { v = *(volatile BYTE *)(g_exe + rva); } __except (EXCEPTION_EXECUTE_HANDLER) { v = -1; }
    return v;
}

static int ComputeMode(void)
{
    if (g_effMenuDetect == 1) {
        int v = ReadExeByte(g_flagRva);
        return v > 0 ? MODE_GAMEPLAY : MODE_MENU;       /* unreadable (-1) => menu => safe pass-through */
    }
    if (g_effMenuDetect == 2) {
        CURSORINFO ci;
        ci.cbSize = sizeof ci;
        if (GetCursorInfo(&ci)) return (ci.flags & CURSOR_SHOWING) ? MODE_MENU : MODE_GAMEPLAY;
        return MODE_MENU;
    }
    return MODE_GAMEPLAY;
}

static void ResetMouse(void);

static int GetMode(void)
{
    LONGLONG now = Now();
    LONG m = g_mode;
    if (m >= 0 && now - g_modeTs < g_freq.QuadPart / 500) return (int)m;    /* cached 2 ms */
    g_modeTs = now;
    {
        LONG nm = ComputeMode();
        LONG old = InterlockedExchange(&g_mode, nm);
        if (old != nm) {
            Log("MODE %s -> %s  flags[%03x]=%d [%03x]=%d [%03x]=%d [%03x]=%d",
                old < 0 ? "init" : old ? "GAMEPLAY" : "MENU", nm ? "GAMEPLAY" : "MENU",
                (unsigned)k_candRva[0], ReadExeByte(k_candRva[0]), (unsigned)k_candRva[1], ReadExeByte(k_candRva[1]),
                (unsigned)k_candRva[2], ReadExeByte(k_candRva[2]), (unsigned)k_candRva[3], ReadExeByte(k_candRva[3]));
            ResetMouse();
        }
        return (int)nm;
    }
}

/* ---------------------------------------------------------------- mouse accumulation / smoothing */
#define NSAMP 256
static volatile LONG g_dx, g_dy;
static CRITICAL_SECTION g_stickCs;
static struct { LONGLONG t; LONG dx, dy; } g_samp[NSAMP];
static int g_head, g_count;
static LONGLONG g_lastCompute;
static short g_outX, g_outY;
static volatile LONG g_pktBias;
static volatile LONG g_padOk;
static volatile LONGLONG g_padOkTs;

/* statistics */
static volatile LONG64 s_rawSeen, s_rawSupp, s_rawDropped, s_calls, s_sumDx, s_sumDy, s_override;
static volatile LONG s_lastOutX, s_lastOutY;
static volatile LONG64 s_giCalls, s_giInjected;
static volatile LONG s_lastType = -1;
static double s_sumRadX, s_sumRadY;          /* only touched by the single consuming player */
static float s_hostSens = 1.0f;
static volatile LONG g_realRX, g_realRY;     /* raw right stick of PadIndex, before our changes */
static volatile LONG g_giHooked;             /* GetInput vtable slot hooked (mode 2) */
static volatile LONG g_giFailed;             /* verification failed: suppression disabled */

/* whether the raw mouse packets should be intercepted/suppressed right now */
static int SuppressOk(void)
{
    return g_modeSel == 1 || g_giHooked;
}

static void SetOut(short x, short y)
{
    if (x != g_outX || y != g_outY) {
        g_outX = x; g_outY = y;
        InterlockedIncrement(&g_pktBias);       /* stick changed -> packet number must change */
    }
}

static void ResetMouse(void)
{
    InterlockedExchange(&g_dx, 0);
    InterlockedExchange(&g_dy, 0);
    EnterCriticalSection(&g_stickCs);
    g_count = 0;
    g_lastCompute = 0;
    SetOut(0, 0);
    LeaveCriticalSection(&g_stickCs);
}

static short ClampS(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32767.0f) return -32767;
    return (short)v;
}

/* recompute the stick from the accumulator at most once per millisecond (shared by all threads) */
static void UpdateStick(LONGLONG now)
{
    double win = g_windowMs / 1000.0, velX, velY, f = (double)g_freq.QuadPart;
    LONG sx = 0, sy = 0;
    int kept = 0, i;
    float mx, my, mm;

    EnterCriticalSection(&g_stickCs);
    if (g_lastCompute && now - g_lastCompute < g_freq.QuadPart / 1000) goto done;
    g_lastCompute = now;
    {
        LONG dx = InterlockedExchange(&g_dx, 0);
        LONG dy = InterlockedExchange(&g_dy, 0);
        if (dx || dy) {
            g_samp[g_head].t = now; g_samp[g_head].dx = dx; g_samp[g_head].dy = dy;
            g_head = (g_head + 1) % NSAMP;
            if (g_count < NSAMP) g_count++;
        }
    }
    for (i = 0; i < g_count; i++) {
        int k = (g_head - 1 - i + NSAMP) % NSAMP;
        if ((double)(now - g_samp[k].t) / f > win) break;
        sx += g_samp[k].dx; sy += g_samp[k].dy; kept++;
    }
    g_count = kept;
    velX = sx / win; velY = sy / win;       /* counts per second */

    mx = (float)(velX / 1000.0) * g_sens * (g_invertX ? -1.0f : 1.0f);
    my = (float)(velY / 1000.0) * g_sens * (g_invertY ? 1.0f : -1.0f);   /* mouse down = +, stick up = + */
    mm = sqrtf(mx * mx + my * my);
    if (mm > 0.01f) {
        float m = mm;
        if (g_curve != 1.0f) m = powf(m, g_curve);
        m = m * (1.0f - g_antiDz) + g_antiDz;       /* lift past the game's dead zone */
        if (m > 1.0f) m = 1.0f;
        SetOut(ClampS(mx * m / mm * 32767.0f), ClampS(my * m / mm * 32767.0f));
    } else {
        SetOut(0, 0);
    }
done:
    LeaveCriticalSection(&g_stickCs);
}

/* ---------------------------------------------------------------- hooks */
static PFN_GetState g_origGetState;
static PFN_GetRawInputData g_origRaw;

static int PadFresh(void)
{
    return g_padOk && Now() - g_padOkTs < g_freq.QuadPart / 2;
}

static void HandleHotkey(void)
{
    static int prev;
    int down = 0;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (down && !prev) {
        LONG nv = !g_enabled;
        InterlockedExchange(&g_enabled, nv);
        ResetMouse();
        Log("F10: Enabled=%ld", nv);
    }
    prev = down;
}

static DWORD WINAPI HookGetState(DWORD user, XSTATE *st)
{
    DWORD r = g_origGetState ? g_origGetState(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    if ((int)user != g_padIndex) return r;
    InterlockedIncrement64(&s_calls);
    if (r != ERROR_SUCCESS || !st) { g_padOk = 0; return r; }
    g_padOkTs = Now();
    g_padOk = 1;
    g_realRX = st->Gamepad.sThumbRX;
    g_realRY = st->Gamepad.sThumbRY;
    HandleHotkey();
    if (g_modeSel != 1) return r;          /* mode 2: stick passes through untouched */
    if (!g_enabled || GetMode() != MODE_GAMEPLAY) return r;

    {
        float sx = st->Gamepad.sThumbRX / 32767.0f;
        float sy = st->Gamepad.sThumbRY / 32767.0f;
        if (sqrtf(sx * sx + sy * sy) > g_override) {
            /* real stick / trackpad wins: pass untouched, drop mouse so it doesn't jump in later */
            InterlockedIncrement64(&s_override);
            ResetMouse();
            st->dwPacketNumber += (DWORD)g_pktBias;
            return r;
        }
    }
    UpdateStick(Now());
    {
        short ox = g_outX, oy = g_outY;      /* same value for every caller within a time slice */
        if (ox || oy) {
            st->Gamepad.sThumbRX = ox;
            st->Gamepad.sThumbRY = oy;
        }
        s_lastOutX = ox; s_lastOutY = oy;
    }
    st->dwPacketNumber += (DWORD)g_pktBias;
    return r;
}

static UINT WINAPI HookGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT pcb, UINT hdr)
{
    UINT r = g_origRaw(h, cmd, data, pcb, hdr);
    if (r == (UINT)-1 || cmd != RID_INPUT || !data || r < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) return r;
    {
        RAWINPUT *ri = (RAWINPUT *)data;
        if (ri->header.dwType != RIM_TYPEMOUSE) return r;
        InterlockedIncrement64(&s_rawSeen);
        if (!g_enabled || !SuppressOk() || !PadFresh() || GetMode() != MODE_GAMEPLAY) return r;
        if (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return r;
        {
            LONG dx = ri->data.mouse.lLastX, dy = ri->data.mouse.lLastY;
            if (!dx && !dy) return r;
            InterlockedAdd(&g_dx, dx);
            InterlockedAdd(&g_dy, dy);
            InterlockedAdd64(&s_sumDx, dx);
            InterlockedAdd64(&s_sumDy, dy);
        }
        if (g_rawSuppress == 2 && ri->data.mouse.usButtonFlags == 0 && ri->data.mouse.usButtonData == 0) {
            InterlockedIncrement64(&s_rawDropped);
            return (UINT)-1;
        }
        if (g_rawSuppress == 1 || g_rawSuppress == 2) {
            ri->data.mouse.lLastX = 0;
            ri->data.mouse.lLastY = 0;
            InterlockedIncrement64(&s_rawSupp);
        }
    }
    return r;
}

/* ---- host input builder hook (mode 2) ----
   bool __fastcall GetInput(void *inputMgr, int player, uint8_t *out, bool flag), pointer in .rdata at RVA 0x346c170.
   out+0 (int32) = device type (0 pad, 1 KB/M); out+0x104/+0x108 = float radians-per-frame mouse yaw/pitch. */
#define GI_SLOT_RVA   0x346c170u
#define GI_SENS_PTR_RVA 0x4001b78u
typedef BYTE (__fastcall *PFN_GetInput)(void *, int, BYTE *, BYTE);
static PFN_GetInput g_origGetInput;
static volatile LONG g_giTried;
static const BYTE k_giPrologue[26] = { 0x48,0x8B,0xC4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0xA0,0x48,0x81,0xEC,0x60,0x01,0x00,0x00 };

static float ReadHostSens(void)
{
    float v = 1.0f;
    __try {
        if (g_exe && GI_SENS_PTR_RVA + 8 <= g_exeImageSize) {
            BYTE *p = *(BYTE **)(g_exe + GI_SENS_PTR_RVA);
            if ((ULONG_PTR)p > 0x10000) {
                float f = *(volatile float *)(p + 0x38);
                if (f >= 0.01f && f <= 100.0f) v = f;      /* NaN fails both comparisons */
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { v = 1.0f; }
    return v;
}

static BYTE __fastcall HookGetInput(void *mgr, int player, BYTE *out, BYTE flag)
{
    BYTE r = g_origGetInput(mgr, player, out, flag);
    if (!r || player != g_padIndex || !out) return r;
    InterlockedIncrement64(&s_giCalls);
    {
        LONG type = *(volatile LONG *)out;
        LONG old = InterlockedExchange(&s_lastType, type);
        if (old != type) Log("input type player %d: %ld -> %ld (0=gamepad 1=KBM)", player, old, type);
        if (type != 0) return r;                  /* KB/M device active: leave host data alone */
    }
    {
        float yaw = 0.0f, pitch = 0.0f;
        if (g_enabled && GetMode() == MODE_GAMEPLAY) {
            LONG dx = InterlockedExchange(&g_dx, 0);
            LONG dy = InterlockedExchange(&g_dy, 0);
            float rx = g_realRX / 32767.0f, ry = g_realRY / 32767.0f;
            if (g_trackpadOv && sqrtf(rx * rx + ry * ry) > g_override) {
                InterlockedIncrement64(&s_override);        /* real stick wins, gyro discarded */
            } else if (dx || dy) {
                float hs = ReadHostSens(), k;
                s_hostSens = hs;
                k = g_gyroSens * hs * 0.022222223f * 0.017453292f;
                yaw   = dx * k * (g_invertX ? -1.0f : 1.0f);
                pitch = dy * k * (g_invertY ? -1.0f : 1.0f);   /* +y = down, as in the host KB/M path */
                s_sumRadX += yaw; s_sumRadY += pitch;
                InterlockedIncrement64(&s_giInjected);
            }
        }
        *(float *)(out + 0x104) = yaw;
        *(float *)(out + 0x108) = pitch;
    }
    return r;
}

static void InstallGetInput(void)
{
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    BYTE *slot, *target = NULL;
    BYTE bytes[32];
    int i, inText = 0, ok = 0;
    char hex[32 * 3 + 1];
    DWORD old;

    if (g_modeSel != 2 || g_giHooked || g_giFailed || !g_exe) return;
    if (g_giTried++ >= 240) {                         /* ~60 s of retries at 250 ms */
        g_giFailed = 1;
        Log("GetInput hook: vtable slot never became non-null, giving up");
        goto fail;
    }
    slot = g_exe + GI_SLOT_RVA;
    if (GI_SLOT_RVA + 8 > g_exeImageSize) { g_giFailed = 1; Log("GetInput hook: slot RVA outside image"); goto fail; }
    __try { target = *(BYTE **)slot; } __except (EXCEPTION_EXECUTE_HANDLER) { target = NULL; }
    if (!target) return;                              /* vtable pointer not filled yet, retry */
    nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        BYTE *s = g_exe + sec->VirtualAddress;
        DWORD sz = sec->Misc.VirtualSize;
        if (!strncmp((char *)sec->Name, ".text", 5) && target >= s && target + 32 <= s + sz) inText = 1;
    }
    memset(bytes, 0, sizeof bytes);
    if (inText) {
        __try { memcpy(bytes, target, 32); ok = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    for (i = 0; i < 32; i++) sprintf(hex + i * 3, "%02X ", bytes[i]);
    Log("GetInput slot RVA=0x%x target=%p (RVA 0x%llx) inText=%d first32: %s", GI_SLOT_RVA, (void *)target,
        (unsigned long long)(target - g_exe), inText, ok ? hex : "(unreadable)");
    if (!inText || !ok || memcmp(bytes, k_giPrologue, sizeof k_giPrologue) != 0) {
        g_giFailed = 1;
        Log("GetInput hook NOT installed: target not in .text or prologue mismatch");
        goto fail;
    }
    g_origGetInput = (PFN_GetInput)target;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        g_giFailed = 1; g_origGetInput = NULL;
        Log("GetInput hook: VirtualProtect failed err=%lu", GetLastError());
        goto fail;
    }
    InterlockedExchangePointer((PVOID volatile *)slot, (PVOID)HookGetInput);
    VirtualProtect(slot, 8, old, &old);
    InterlockedExchange(&g_giHooked, 1);
    Log("GetInput hook INSTALLED: slot %p orig=%p -> hook=%p", (void *)slot, (void *)target, (void *)HookGetInput);
    return;
fail:
    Log("FALLBACK: raw mouse suppression disabled, game keeps native KB/M behaviour");
}

/* ---- window subclass ---- */
static HWND g_wnd;
static WNDPROC g_origWndProc;
static char g_wndClass[128];

static LRESULT CALLBACK HookWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_MOUSEMOVE && g_blockMove && g_enabled && SuppressOk() && PadFresh() && GetMode() == MODE_GAMEPLAY) return 0;
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    char cls[128];
    HWND *best = (HWND *)lp;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    cls[0] = 0;
    GetClassNameA(h, cls, sizeof cls);
    if (!_stricmp(cls, "UnrealWindow")) { *best = h; return FALSE; }
    if (!*best) {
        RECT rc;
        GetClientRect(h, &rc);
        if (rc.right > 200 && rc.bottom > 200) *best = h;
    }
    return TRUE;
}

static int InstallWindow(void)
{
    HWND best = NULL;
    if (g_wnd) return 1;
    EnumWindows(EnumProc, (LPARAM)&best);
    if (!best) return 0;
    GetClassNameA(best, g_wndClass, sizeof g_wndClass);
    if (!IsWindowUnicode(best)) { Log("window %p class '%s' is ANSI, not subclassed", (void *)best, g_wndClass); g_wnd = best; return 1; }
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(best, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
    if (!g_origWndProc) { Log("SetWindowLongPtr failed on window %p class '%s' err=%lu", (void *)best, g_wndClass, GetLastError()); return 0; }
    g_wnd = best;
    Log("window subclassed: hwnd=%p class='%s' origProc=%p", (void *)best, g_wndClass, (void *)g_origWndProc);
    return 1;
}

/* ---- IAT patching ---- */
static volatile LONG g_xiPatched, g_rawPatched;

static int PatchSlot(ULONG_PTR *slot, ULONG_PTR newval)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    *slot = newval;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

static int InstallIat(void)
{
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *d;
    if (!g_exe) return 0;
    nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(g_exe + dir->VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)(g_exe + d->Name);
        ULONG_PTR *oft = (ULONG_PTR *)(g_exe + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        ULONG_PTR *ft = (ULONG_PTR *)(g_exe + d->FirstThunk);
        int i;
        int isXi = !_stricmp(dll, "xinput1_3.dll");
        int isUser = !_stricmp(dll, "user32.dll");
        if (!isXi && !isUser) continue;
        for (i = 0; oft[i]; i++) {
            if (isXi && !g_xiPatched && (oft[i] & IMAGE_ORDINAL_FLAG64) && (oft[i] & 0xFFFF) == 2) {
                ULONG_PTR cur = ft[i];
                ULONGLONG rva = (ULONGLONG)((BYTE *)&ft[i] - g_exe);
                g_origGetState = (PFN_GetState)cur;
                if (PatchSlot(&ft[i], (ULONG_PTR)HookGetState)) {
                    InterlockedExchange(&g_xiPatched, 1);
                    Log("IAT XInputGetState (xinput1_3 #2): slot VA=0x%llx RVA=0x%llx (%s expected 0x%llx) orig=%p ourExport=%p -> hook=%p",
                        (ULONGLONG)(ULONG_PTR)&ft[i], rva, rva == XINPUT_GETSTATE_SLOT_RVA ? "==" : "!=",
                        XINPUT_GETSTATE_SLOT_RVA, (void *)cur, (void *)p_GetState, (void *)HookGetState);
                } else {
                    g_origGetState = NULL;
                    Log("IAT XInputGetState: VirtualProtect failed err=%lu", GetLastError());
                }
            }
            if (isUser && !g_rawPatched && !(oft[i] & IMAGE_ORDINAL_FLAG64)) {
                IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(g_exe + (oft[i] & 0x7FFFFFFFFFFFFFFFULL));
                if (!strcmp((char *)ibn->Name, "GetRawInputData")) {
                    ULONG_PTR cur = ft[i];
                    g_origRaw = (PFN_GetRawInputData)cur;
                    if (PatchSlot(&ft[i], (ULONG_PTR)HookGetRawInputData)) {
                        InterlockedExchange(&g_rawPatched, 1);
                        Log("IAT GetRawInputData (user32): slot VA=0x%llx RVA=0x%llx orig=%p -> hook=%p",
                            (ULONGLONG)(ULONG_PTR)&ft[i], (ULONGLONG)((BYTE *)&ft[i] - g_exe), (void *)cur, (void *)HookGetRawInputData);
                    } else {
                        g_origRaw = NULL;
                        Log("IAT GetRawInputData: VirtualProtect failed err=%lu", GetLastError());
                    }
                }
            }
        }
    }
    return g_xiPatched && g_rawPatched;
}

static CRITICAL_SECTION g_instCs;
static volatile LONG g_lazyTried;

static void TryInstall(int withWindow)
{
    EnterCriticalSection(&g_instCs);
    if (!(g_xiPatched && g_rawPatched)) InstallIat();
    if (withWindow && !g_wnd) InstallWindow();
    if (withWindow) InstallGetInput();
    LeaveCriticalSection(&g_instCs);
}

static void EnsureInit(void);
static void EnsureInstalled(void)       /* called lazily from every export, first call only does work */
{
    EnsureInit();
    if (InterlockedExchange(&g_lazyTried, 1)) return;
    TryInstall(0);
}

/* ---- worker thread: retries installation, then logs status ---- */
static DWORD WINAPI WorkerThread(LPVOID unused)
{
    int tick;
    LONG64 last[6] = { -1, -1, -1, -1, -1, -1 };
    (void)unused;
    EnsureInit();
    for (tick = 0; tick < 1200 && !(g_xiPatched && g_rawPatched && g_wnd && (g_modeSel != 2 || g_giHooked || g_giFailed)); tick++) {       /* up to ~5 min at 250 ms */
        Sleep(250);
        TryInstall(1);
        if (tick == 80) {
            if (!g_xiPatched) Log("WARNING: XInputGetState IAT not patched after 20 s");
            if (!g_rawPatched) Log("WARNING: GetRawInputData IAT not patched after 20 s");
        }
    }
    if (!g_wnd) Log("WARNING: game window not found / not subclassed");
    Log("init complete: xinputIAT=%ld rawIAT=%ld window=%p", g_xiPatched, g_rawPatched, (void *)g_wnd);
    for (;;) {
        LONG64 cur[6];
        Sleep(1000);
        cur[0] = s_rawSeen; cur[1] = s_rawSupp; cur[2] = s_calls; cur[3] = s_sumDx; cur[4] = s_sumDy; cur[5] = s_rawDropped + s_override + s_giCalls + s_giInjected;
        if (!memcmp(cur, last, sizeof cur)) continue;
        memcpy(last, cur, sizeof cur);
        Log("STATUS enabled=%ld mode=%s raw seen=%lld supp=%lld dropped=%lld | sum dx=%lld dy=%lld | xinput calls=%lld override=%lld | out=(%ld,%ld) | flags %d %d %d %d",
            g_enabled, g_mode == 1 ? "GAMEPLAY" : "MENU", s_rawSeen, s_rawSupp, s_rawDropped, s_sumDx, s_sumDy, s_calls, s_override,
            s_lastOutX, s_lastOutY,
            ReadExeByte(k_candRva[0]), ReadExeByte(k_candRva[1]), ReadExeByte(k_candRva[2]), ReadExeByte(k_candRva[3]));
        Log("STATUS2 Mode=%d giHooked=%ld getinput calls=%lld injected=%lld sumRad=(%.5f,%.5f) hostSens=%.3f type(p%d)=%ld",
            g_modeSel, g_giHooked, s_giCalls, s_giInjected, s_sumRadX, s_sumRadY, (double)s_hostSens, g_padIndex, s_lastType);
    }
    return 0;
}

/* ---------------------------------------------------------------- exports */
DWORD WINAPI XInputGetState(DWORD user, XSTATE *st)
{
    EnsureInstalled();
    return p_GetState ? p_GetState(user, st) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD user, void *v)
{
    EnsureInstalled();
    return p_SetState ? p_SetState(user, v) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD user, DWORD flags, void *c)
{
    EnsureInstalled();
    return p_GetCaps ? p_GetCaps(user, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI XInputEnable(BOOL e)
{
    EnsureInstalled();
    if (p_Enable) p_Enable(e);
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD user, LPWSTR a, UINT *an, LPWSTR b, UINT *bn)
{
    EnsureInstalled();
    return p_Guids ? p_Guids(user, a, an, b, bn) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD user, BYTE type, void *b)
{
    EnsureInstalled();
    return p_Battery ? p_Battery(user, type, b) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD user, DWORD reserved, void *k)
{
    EnsureInstalled();
    return p_Keystroke ? p_Keystroke(user, reserved, k) : ERROR_DEVICE_NOT_CONNECTED;
}

/* ---------------------------------------------------------------- init */
static void LoadReal(void)
{
    static const char *names[] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" };
    char sys[MAX_PATH];
    int i;
    GetSystemDirectoryA(sys, MAX_PATH);
    for (i = 0; i < 3 && !g_real; i++) {
        _snprintf(g_realPath, MAX_PATH, "%s\\%s", sys, names[i]);
        g_real = LoadLibraryA(g_realPath);
    }
    if (!g_real) { g_realPath[0] = 0; return; }
    p_GetState  = (PFN_GetState)GetProcAddress(g_real, "XInputGetState");
    p_SetState  = (PFN_SetState)GetProcAddress(g_real, "XInputSetState");
    p_GetCaps   = (PFN_GetCaps)GetProcAddress(g_real, "XInputGetCapabilities");
    p_Enable    = (PFN_Enable)GetProcAddress(g_real, "XInputEnable");
    p_Guids     = (PFN_Guids)GetProcAddress(g_real, "XInputGetDSoundAudioDeviceGuids");
    p_Battery   = (PFN_Battery)GetProcAddress(g_real, "XInputGetBatteryInformation");
    p_Keystroke = (PFN_Keystroke)GetProcAddress(g_real, "XInputGetKeystroke");
}

static volatile LONG g_inited;
static CRITICAL_SECTION g_initCs;

static void InitAll(HINSTANCE inst)
{
    char *p;
    HANDLE fh;
    LARGE_INTEGER sz;
    IMAGE_NT_HEADERS *nt;

    LoadReal();

    GetModuleFileNameA(inst, g_ini, MAX_PATH);
    p = strrchr(g_ini, '\\');
    if (p) {
        strcpy(g_logPath, g_ini);
        strcpy(strrchr(g_logPath, '\\') + 1, "mixed_input.log");
        strcpy(p + 1, "xinput_mouse.ini");
    }
    LoadSettings();
    if (g_logOn && g_logPath[0]) g_logFile = fopen(g_logPath, "w");

    g_exe = (BYTE *)GetModuleHandleA(NULL);
    nt = (IMAGE_NT_HEADERS *)(g_exe + ((IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
    g_exeImageSize = nt->OptionalHeader.SizeOfImage;
    GetModuleFileNameA(NULL, g_exePath, MAX_PATH);
    fh = CreateFileA(g_exePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (fh != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(fh, &sz)) g_exeFileSize = (ULONGLONG)sz.QuadPart;
        CloseHandle(fh);
    }

    g_effMenuDetect = g_menuDetect;
    if (g_menuDetect == 1 && g_exeFileSize != EXPECTED_EXE_SIZE) g_effMenuDetect = 2;

    Log("mixed_input proxy loaded (x64). exe='%s' base=%p imageSize=0x%lx fileSize=%llu (expected %llu)",
        g_exePath, (void *)g_exe, g_exeImageSize, g_exeFileSize, EXPECTED_EXE_SIZE);
    Log("real xinput: '%s' handle=%p GetState=%p", g_realPath, (void *)g_real, (void *)p_GetState);
    Log("mode=%d GyroSens=%.3f TrackpadOverride=%d", g_modeSel, (double)g_gyroSens, g_trackpadOv);
    Log("settings: Enabled=%ld PadIndex=%d Sens=%.3f Curve=%.2f SmoothMs=%.0f AntiDz=%.2f Override=%.2f InvY=%d InvX=%d RawSuppress=%d BlockMove=%d MenuDetect=%d(effective %d) FlagRva=0x%lx",
        g_enabled, g_padIndex, g_sens, g_curve, g_windowMs, g_antiDz, g_override, g_invertY, g_invertX, g_rawSuppress, g_blockMove,
        g_menuDetect, g_effMenuDetect, (unsigned long)g_flagRva);
    if (g_menuDetect == 1 && g_effMenuDetect != 1)
        Log("exe size mismatch: MenuDetect=1 unsafe for this build, falling back to cursor heuristic (2)");
}

static void EnsureInit(void)        /* heavy init, never under loader lock */
{
    if (g_inited) return;
    EnterCriticalSection(&g_initCs);
    if (!g_inited) { InitAll(g_self); g_inited = 1; }
    LeaveCriticalSection(&g_initCs);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_self = inst;
        QueryPerformanceFrequency(&g_freq);
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_stickCs);
        InitializeCriticalSection(&g_instCs);
        InitializeCriticalSection(&g_initCs);
        CloseHandle(CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL));
    }
    return TRUE;
}
