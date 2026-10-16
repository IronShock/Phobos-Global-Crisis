// Port of MO_ExperienceEnhancements' GUI/GdiText.cpp into Phobos.
// See MOEE.GdiText.h for the porting notes.

#include "MOEE.GdiText.h"

#include <Utilities/Debug.h>

// ---------------- no-CRT string helpers ----------------

unsigned UtoA(unsigned v, char* p)
{
    char tmp[12];
    int i = 0, j = 0;
    if (!v) tmp[i++] = '0';
    while (v && i < 11) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) p[j++] = tmp[--i];
    p[j] = 0;
    return (unsigned)j;
}

unsigned HtoA(unsigned v, char* p)
{
    static const char* hex = "0123456789ABCDEF";
    unsigned n = 0;
    int i;
    p[n++] = '0'; p[n++] = 'x';
    for (i = 28; i >= 0; i -= 4) p[n++] = hex[(v >> i) & 0xF];
    p[n] = 0;
    return n;
}

unsigned AppendCStr(char* dst, unsigned pos, const char* src, unsigned cap)
{
    while (*src && pos < cap - 1) dst[pos++] = *src++;
    dst[pos] = 0;
    return pos;
}

unsigned AppendCh(char* dst, unsigned pos, char c, unsigned cap)
{
    if (pos < cap - 1) { dst[pos++] = c; dst[pos] = 0; }
    return pos;
}

bool IsPlausiblePtr(const void* p)
{
    unsigned u = (unsigned)(size_t)p;
    return (u >= 0x00010000u && u < 0x7FF00000u && (u & 3u) == 0);
}

// ---------------- logging ----------------

int g_LogDetail = 0;

void LogLine(const char* body)
{
    Debug::Log("[MOEE:gdi] %s\n", body ? body : "");
}

// ---------------- game window and mouse coordinates ----------------
// WWMouse reports window/client pixels; when the window and the game resolution
// differ (stretched presentation) they must be remapped or every hit test is
// offset. The basis must be the full frame width (Primary), not Composite:
// at 1920x1080 Composite is only 1752 wide (the right 168px is its own sidebar
// surface), so using it as the denominator would shrink cursor x by 1752/1920.

static HWND g_GameWnd = NULL;
static int g_WinW = 0, g_WinH = 0;
static int g_GameW = 0, g_GameH = 0;

int GtGameW(void) { return g_GameW; }
int GtGameH(void) { return g_GameH; }
void GtSetGameSize(int w, int h) { g_GameW = w; g_GameH = h; }

static int __stdcall GtEnumGameWnd(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    RECT rc;
    if (!IsWindowVisible(h)) return 1;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return 1;
    if (!GetClientRect(h, &rc)) return 1;
    if (rc.right < 100 || rc.bottom < 100) return 1;
    *(HWND*)lp = h;
    return 0;
}

void GtRefreshWinSize(unsigned tick)
{
    static unsigned s_last = 0;
    RECT rc;
    if (tick - s_last < 60) return;
    s_last = tick;
    if (!g_GameWnd || !IsWindow(g_GameWnd))
    {
        g_GameWnd = NULL;
        EnumWindows((WNDENUMPROC)GtEnumGameWnd, (LPARAM)&g_GameWnd);
    }
    if (g_GameWnd && GetClientRect(g_GameWnd, &rc) && rc.right > 100 && rc.bottom > 100)
    {
        if (rc.right != g_WinW || rc.bottom != g_WinH)
        {
            char wb[128], w1[16], w2[16];
            unsigned q = 0;
            g_WinW = rc.right; g_WinH = rc.bottom;
            q = AppendCStr(wb, q, "win: client=", sizeof(wb));
            UtoA((unsigned)g_WinW, w1); q = AppendCStr(wb, q, w1, sizeof(wb));
            q = AppendCh(wb, q, 'x', sizeof(wb));
            UtoA((unsigned)g_WinH, w2); q = AppendCStr(wb, q, w2, sizeof(wb));
            LogLine(wb);
        }
    }
}

int GtGameFocused(void)
{
    if (!g_GameWnd) return 0;
    return (GetForegroundWindow() == g_GameWnd) ? 1 : 0;
}

int GtMouseGameXY(int* x, int* y)
{
    void* m = *(void**)0x00887640u;   /* WWMouse::Instance */
    int mx, my;
    if (!m || !IsPlausiblePtr(m)) return 0;
    mx = *(int*)((char*)m + 0x1C);
    my = *(int*)((char*)m + 0x20);
    if (g_WinW > 100 && g_WinH > 100 && g_GameW > 100 && g_GameH > 100)
    {
        *x = mx * g_GameW / g_WinW;
        *y = my * g_GameH / g_WinH;
    }
    else { *x = mx; *y = my; }
    return 1;
}

// ---------------- font resolution ----------------

void CopyFace(wchar_t* dst, const wchar_t* src)
{
    int i = 0;
    while (src[i] && i < FONT_FACE_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int FaceSame(const wchar_t* a, const wchar_t* b)
{
    int i = 0;
    while (i < FONT_FACE_MAX - 1 && a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static unsigned RdU16BE(const unsigned char* p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static unsigned RdU32BE(const unsigned char* p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static int ReadAt(HANDLE h, unsigned off, unsigned char* buf, unsigned n)
{
    DWORD got = 0;
    if (off >= 0x80000000u) return 0;
    if (SetFilePointer(h, (LONG)off, NULL, FILE_BEGIN) == (DWORD)-1) return 0;
    if (!ReadFile(h, buf, n, &got, NULL) || got != n) return 0;
    return 1;
}

// Read the family name (nameID=1) from a font file: ttf/otf and ttc (first
// font) both work. Prefers the Windows English name (3,1,0409), then Chinese
// (3,1,0804), then anything else. Returns 1 (name in `out`) or 0.
static int ParseFamilyName(const char* path, wchar_t* out)
{
    HANDLE h;
    DWORD fsize;
    unsigned char head[16];
    unsigned fontOff = 0, nTab, i;
    unsigned char dir[12 + 64 * 16];
    unsigned nameOff = 0, nameLen = 0;
    unsigned char nh[6];
    unsigned count, strOff, r;
    int best = 99;
    unsigned bestLen = 0, bestStr = 0, bestPlat = 0;
    unsigned char rec[12];
    unsigned char sbuf[FONT_FACE_MAX * 2];
    unsigned want, k, c;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    fsize = GetFileSize(h, NULL);
    if (fsize == (DWORD)-1 || fsize < 16) { CloseHandle(h); return 0; }
    if (!ReadAt(h, 0, head, 16)) { CloseHandle(h); return 0; }
    if (head[0] == 't' && head[1] == 't' && head[2] == 'c' && head[3] == 'f')
    {
        unsigned nFonts = RdU32BE(head + 8);
        if (nFonts < 1) { CloseHandle(h); return 0; }
        fontOff = RdU32BE(head + 12);
        if (fontOff + 12 > fsize || fontOff + 12 < fontOff) { CloseHandle(h); return 0; }
    }
    if (!ReadAt(h, fontOff, dir, 12)) { CloseHandle(h); return 0; }
    nTab = RdU16BE(dir + 4);
    if (nTab < 1 || nTab > 64) { CloseHandle(h); return 0; }
    if (nTab * 16 > fsize - fontOff - 12) { CloseHandle(h); return 0; }
    if (!ReadAt(h, fontOff + 12, dir + 12, nTab * 16)) { CloseHandle(h); return 0; }
    for (i = 0; i < nTab; i++)
    {
        unsigned char* t = dir + 12 + i * 16;
        unsigned to, ln;
        if (t[0] == 'n' && t[1] == 'a' && t[2] == 'm' && t[3] == 'e')
        {
            to = RdU32BE(t + 8);
            ln = RdU32BE(t + 12);
            if (to >= fsize || ln > fsize - to || ln < 6) continue;
            nameOff = to;
            nameLen = ln;
            break;
        }
    }
    if (!nameOff) { CloseHandle(h); return 0; }
    if (!ReadAt(h, nameOff, nh, 6)) { CloseHandle(h); return 0; }
    count = RdU16BE(nh + 2);
    strOff = RdU16BE(nh + 4);
    if (count > 300 || strOff >= nameLen) { CloseHandle(h); return 0; }
    for (r = 0; r < count; r++)
    {
        unsigned plat, lang, nid, len, off;
        int score;
        if (!ReadAt(h, nameOff + 6 + r * 12, rec, 12)) break;
        plat = RdU16BE(rec + 0);
        lang = RdU16BE(rec + 4);
        nid  = RdU16BE(rec + 6);
        len  = RdU16BE(rec + 8);
        off  = RdU16BE(rec + 10);
        if (nid != 1 || !len) continue;
        if (off >= nameLen - strOff || len > nameLen - strOff - off) continue;
        if (plat == 3) score = (lang == 0x0409) ? 0 : (lang == 0x0804) ? 1 : 2;
        else if (plat == 1) score = 3;
        else continue;
        if (score < best) { best = score; bestLen = len; bestStr = strOff + off; bestPlat = plat; }
        if (best == 0) break;
    }
    if (best > 3 || !bestLen) { CloseHandle(h); return 0; }
    want = bestLen;
    if (want > (FONT_FACE_MAX - 1) * 2) want = (FONT_FACE_MAX - 1) * 2;
    if (!ReadAt(h, nameOff + bestStr, sbuf, want)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    k = 0;
    if (bestPlat == 3)
    {
        for (c = 0; c + 1 < want && k < FONT_FACE_MAX - 1; c += 2)
            out[k++] = (wchar_t)(((unsigned)sbuf[c] << 8) | sbuf[c + 1]);
    }
    else
    {
        for (c = 0; c < want && k < FONT_FACE_MAX - 1; c++)
            out[k++] = (wchar_t)sbuf[c];
    }
    out[k] = 0;
    return k > 0;
}

int FileExists(const char* path)
{
    DWORD a;
    if (!path || !*path) return 0;
    a = GetFileAttributesA(path);
    return (a != (DWORD)-1 && !(a & FILE_ATTRIBUTE_DIRECTORY));
}

// Search game folder -> system font folder (verbatim match).
// Writes the full path into outPath, returns 1=game folder, 2=system, 0=neither.
int FindFontFile(const char* name, char* outPath, int cap, const char** tag)
{
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    DWORD i;
    unsigned k;
    char dir[MAX_PATH];
    char base[MAX_PATH];
    unsigned blen = 0;

    if (name && *name)
    {
        if (n > 0 && n < MAX_PATH)
        {
            i = n;
            while (i > 0 && exe[i] != '\\' && exe[i] != '/') i--;
            k = 0;
            while (k < i && k + 1 < (unsigned)cap) { outPath[k] = exe[k]; k++; }
            outPath[k] = 0;
            k = AppendCStr(outPath, k, "\\", (unsigned)cap);
            k = AppendCStr(outPath, k, name, (unsigned)cap);
            if (FileExists(outPath)) { if (tag) *tag = "cur"; return 1; }

            /* Game-dir subfolders: the CnCNet client keeps its TTFs under
             * Resources\... so they are reachable without copying them out. */
            blen = i;
            if (blen >= (unsigned)sizeof(base)) blen = (unsigned)sizeof(base) - 1;
            for (k = 0; k < blen; k++) base[k] = exe[k];
            base[blen] = 0;
        }
        {
            static const char* subs[] = {
                "\\Resources\\Translations\\zh-Hans\\Fonts\\",
                "\\Resources\\Fonts\\",
                "\\Fonts\\",
            };
            unsigned s;

            for (s = 0; s < sizeof(subs) / sizeof(subs[0]); s++)
            {
                if (!base[0]) break;
                k = AppendCStr(outPath, 0, base, (unsigned)cap);
                k = AppendCStr(outPath, k, subs[s], (unsigned)cap);
                k = AppendCStr(outPath, k, name, (unsigned)cap);
                if (FileExists(outPath)) { if (tag) *tag = "cur"; return 1; }
            }
        }
        if (GetWindowsDirectoryA(dir, MAX_PATH))
        {
            k = AppendCStr(outPath, 0, dir, (unsigned)cap);
            k = AppendCStr(outPath, k, "\\Fonts\\", (unsigned)cap);
            k = AppendCStr(outPath, k, name, (unsigned)cap);
            if (FileExists(outPath)) { if (tag) *tag = "sys"; return 2; }
        }
    }
    if (tag) *tag = "none";
    return 0;
}

static void LogFace(const char* tag, const char* what, const char* v)
{
    char logb[256];
    unsigned q = 0;
    q = AppendCStr(logb, q, "font: ", sizeof(logb));
    q = AppendCStr(logb, q, tag, sizeof(logb));
    q = AppendCStr(logb, q, what, sizeof(logb));
    q = AppendCStr(logb, q, v, sizeof(logb));
    LogLine(logb);
}

// face -> ASCII ('?' for non-ASCII); returns 1 if fully ASCII.
static int FaceToAscii(const wchar_t* face, char* out, int cap)
{
    int i = 0, ascii = 1;
    while (face[i] && i < cap - 1)
    {
        wchar_t wc = face[i];
        if (wc >= 32 && wc < 127) out[i] = (char)wc;
        else { out[i] = '?'; ascii = 0; }
        i++;
    }
    out[i] = 0;
    return ascii;
}

static void LogFaceOne(const char* tag, const char* file, const char* found,
                       const wchar_t* face, const char* tail)
{
    char logb[256], tmp[FONT_FACE_MAX];
    unsigned q = 0;
    int ascii = 1, k;
    q = AppendCStr(logb, q, "font: ", sizeof(logb));
    q = AppendCStr(logb, q, tag, sizeof(logb));
    q = AppendCStr(logb, q, " file=", sizeof(logb));
    q = AppendCStr(logb, q, file, sizeof(logb));
    q = AppendCStr(logb, q, " found=", sizeof(logb));
    q = AppendCStr(logb, q, found, sizeof(logb));
    if (face)
    {
        ascii = FaceToAscii(face, tmp, sizeof(tmp));
        q = AppendCStr(logb, q, " use=", sizeof(logb));
        q = AppendCStr(logb, q, tmp, sizeof(logb));
    }
    if (tail)
    {
        q = AppendCStr(logb, q, " ", sizeof(logb));
        q = AppendCStr(logb, q, tail, sizeof(logb));
    }
    LogLine(logb);
    if (face && !ascii)
    {
        char hx[64];
        q = 0;
        k = 0;
        while (face[k] && k < 8 && q + 6 < sizeof(hx))
        {
            unsigned w = (unsigned)face[k] & 0xFFFFu;
            int sh;
            hx[q++] = 'U'; hx[q++] = '+';
            for (sh = 12; sh >= 0; sh -= 4)
                hx[q++] = "0123456789ABCDEF"[(w >> sh) & 15u];
            hx[q++] = ' ';
            k++;
        }
        hx[q] = 0;
        LogFace(tag, " useHex=", hx);
    }
}

static void ResolveFontFaceRaw(const char* cfg, const char* tag, wchar_t* out,
                     const wchar_t* fallback)
{
    char tmp[64], path[MAX_PATH * 2];
    int j = 0;
    const char* where = "none";
    if (!cfg) { CopyFace(out, fallback); return; }
    while (cfg[j] && j < (int)sizeof(tmp) - 1) { tmp[j] = cfg[j]; j++; }
    tmp[j] = 0;
    if (!tmp[0]) { CopyFace(out, fallback); return; }
    if (!FindFontFile(tmp, path, sizeof(path), &where))
    {
        LogFaceOne(tag, tmp, "none", NULL, "fallback");
        CopyFace(out, fallback);
        return;
    }
    if (ParseFamilyName(path, out))
    {
        AddFontResourceExA(path, FR_PRIVATE, 0);
        LogFaceOne(tag, tmp, where, out, NULL);
        return;
    }
    LogFaceOne(tag, tmp, where, NULL, "parse-failed fallback");
    CopyFace(out, fallback);
}

// CJK coverage probe for a candidate family: 一(U+4E2D) / ：(U+FF1A) must both
// have glyphs. Pure Win32, no game dependency.
static int FaceHasCJK(const wchar_t* face)
{
    static const wchar_t probe[3] = { 0x4E2Du, 0xFF1Au, 0 };
    HDC screen, dc;
    HFONT f, old;
    WORD idx[2];
    DWORD n;
    int ok = 0;
    if (!face || !face[0]) return 0;
    screen = GetDC(NULL);
    if (!screen) return 0;
    dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!dc) return 0;
    f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (!f) { DeleteDC(dc); return 0; }
    old = (HFONT)SelectObject(dc, f);
    n = GetGlyphIndicesW(dc, probe, 2, idx, 0);
    if (n == 2 && idx[0] != 0xFFFFu && idx[1] != 0xFFFFu) ok = 1;
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    return ok;
}

void ResolveFontFace(const char* cfg, const char* tag, wchar_t* out,
                     const wchar_t* fallback)
{
    static const wchar_t* cands[] = {
        L"Microsoft YaHei", L"SimSun", L"NSimSun", L"SimHei"
    };
    unsigned i;
    ResolveFontFaceRaw(cfg, tag, out, fallback);
    if (FaceHasCJK(out)) return;
    for (i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
    {
        if (FaceSame(cands[i], out)) continue;
        if (FaceHasCJK(cands[i]))
        {
            char cn[FONT_FACE_MAX];
            FaceToAscii(cands[i], cn, sizeof(cn));
            LogFace(tag, " cjkHeal=", cn);
            CopyFace(out, cands[i]);
            return;
        }
    }
}

// ---------------- surface vtable wrappers ----------------

typedef int (__fastcall *GtGetIntFn)(void* s, void* edx);
typedef int (__fastcall *GtFillFn)(void* s, void* edx, RectangleStruct*, ColorStruct*, int);
typedef void* (__fastcall *GtLockFn)(void* s, void* edx, int, int);
typedef int (__fastcall *GtUnlockFn)(void* s, void* edx);

static void* GtVtable(void* surf, unsigned vtOff)
{
    unsigned char* vt;
    if (!surf || !IsPlausiblePtr(surf)) return NULL;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return NULL;
    return *(void**)(vt + vtOff);
}

int GtSurfGet(void* surf, unsigned vtOff)
{
    GtGetIntFn fn = (GtGetIntFn)GtVtable(surf, vtOff);
    if (!fn) return -1;
    return fn(surf, NULL);
}

void GtFillRectTrans(void* surf, RectangleStruct* r, ColorStruct* c, int opacity)
{
    GtFillFn fn = (GtFillFn)GtVtable(surf, GtVT_FILL_RECT_TRANS);
    if (!fn) return;
    fn(surf, NULL, r, c, opacity);
}

unsigned char* GtLock(void* surf)
{
    GtLockFn fn = (GtLockFn)GtVtable(surf, GtVT_LOCK);
    unsigned char* p;
    if (!fn) return NULL;
    if (!IsPlausiblePtr((const void*)fn)) return NULL;
    p = (unsigned char*)fn(surf, NULL, 0, 0);
    return (p && IsPlausiblePtr(p)) ? p : NULL;
}

void GtUnlock(void* surf)
{
    GtUnlockFn fn = (GtUnlockFn)GtVtable(surf, GtVT_UNLOCK);
    if (!fn) return;
    if (!IsPlausiblePtr((const void*)fn)) return;
    fn(surf, NULL);
}

void GtDrawOutline(void* surf, int x, int y, int w, int h,
                   unsigned char r, unsigned char g, unsigned char b, int op)
{
    RectangleStruct e;
    ColorStruct c;
    if (w < 8 || h < 8) return;
    c.R = r; c.G = g; c.B = b;
    e.X = x; e.Y = y; e.Width = w; e.Height = 2;
    GtFillRectTrans(surf, &e, &c, op);
    e.Y = y + h - 2;
    GtFillRectTrans(surf, &e, &c, op);
    e.Y = y + 2; e.Height = h - 4; e.Width = 2;
    GtFillRectTrans(surf, &e, &c, op);
    e.X = x + w - 2;
    GtFillRectTrans(surf, &e, &c, op);
}

int GtBitLineHeight(void)
{
    void* font = *(void**)ADDR_BITFONT_INSTANCE;
    int h = font ? *(int*)((char*)font + FONT_OFF_HEIGHT) : 14;
    if (h < 6 || h > 64) h = 14;
    return h;
}

int GtScreenW(void)
{
    RectangleStruct* vb = (RectangleStruct*)ADDR_SURF_VIEWBOUNDS;
    return (vb && vb->Width > 200) ? vb->Width : 640;
}

// ---------------- engine clock ----------------
// clock = [0x887340] + (Tick() - [0x887338]), Tick = timeGetTime()>>4.
// [0x887338]==-1 means no tick compensation (paused / not started).

#define ADDR_TICK_FN   0x006C8C40u
#define ADDR_CLK_REF   0x00887338u
#define ADDR_CLK_BASE  0x00887340u
#define ADDR_CLK_ARG   0x0088733Cu

typedef unsigned (__fastcall *GtTickFn)(void* a, void* edxUnused);

unsigned GtTickNow(void)
{
    static GtTickFn fn = NULL;
    if (!fn) fn = (GtTickFn)ADDR_TICK_FN;
    return fn((void*)ADDR_CLK_ARG, NULL);
}

unsigned GtClockNow(void)
{
    unsigned ref = *(unsigned*)ADDR_CLK_REF;
    unsigned b = *(unsigned*)ADDR_CLK_BASE;
    if (ref != 0xFFFFFFFFu) b += GtTickNow() - ref;
    return b;
}

// ---------------- pixel format calibration ----------------

int g_RBits = 5, g_GBits = 6, g_BBits = 5;  /* default 565 */
int g_Calibrated = 0;

unsigned char g_ExpR[32], g_ExpG[64], g_ExpB[32];
int g_ExpReady = 0;

void GtBuildExpand(void)
{
    int i, rN = (1 << g_RBits) - 1, gN = (1 << g_GBits) - 1, bN = (1 << g_BBits) - 1;
    if (rN < 1) rN = 31;
    if (gN < 1) gN = 63;
    if (bN < 1) bN = 31;
    for (i = 0; i < 32; i++) g_ExpR[i] = (unsigned char)((i * 255 + rN / 2) / rN);
    for (i = 0; i < 32; i++) g_ExpB[i] = (unsigned char)((i * 255 + bN / 2) / bN);
    for (i = 0; i < 64; i++)
    {
        unsigned v = (unsigned)((i * 255 + gN / 2) / gN);
        g_ExpG[i] = (unsigned char)(v > 255u ? 255u : v);
    }
    g_ExpReady = 1;
}

void Calibrate(void)
{
    void* tmp;
    unsigned char* vt;
    RectangleStruct rc;
    char b1[16], buf[256];
    unsigned p;
    typedef int (__fastcall *FillFn)(void*, void*, RectangleStruct*, unsigned);
    typedef unsigned (__fastcall *GetPxFn)(void*, void*, Point2D*);
    FillFn fill;
    GetPxFn getpx;
    Point2D pt;
    unsigned raw;

    if (g_Calibrated) return;

    tmp = *(void**)ADDR_SURF_TEMP;
    if (!tmp || !IsPlausiblePtr(tmp)) { LogLine("cal: no temp surface, assume 565"); g_Calibrated = 1; return; }
    vt = *(unsigned char**)tmp;
    if (!vt || !IsPlausiblePtr(vt)) { LogLine("cal: bad temp vtable, assume 565"); g_Calibrated = 1; return; }

    rc.X = 0; rc.Y = 0; rc.Width = 4; rc.Height = 4;
    fill = (FillFn)(*(void**)(vt + GtVT_FILL_RECT));
    getpx = (GetPxFn)(*(void**)(vt + GtVT_GET_PIXEL));
    if (!fill || !getpx) { LogLine("cal: no fill/getpx, assume 565"); g_Calibrated = 1; return; }

    fill(tmp, NULL, &rc, 0x000000FFu);   /* COLORREF pure red */
    pt.X = 1; pt.Y = 1;
    raw = getpx(tmp, NULL, &pt) & 0xFFFFu;

    if ((raw & 0xF800u) == 0xF800u)      { g_RBits = 5; g_GBits = 6; g_BBits = 5; }
    else if ((raw & 0x7C00u) == 0x7C00u) { g_RBits = 5; g_GBits = 5; g_BBits = 5; }
    g_ExpReady = 0;

    p = 0;
    p = AppendCStr(buf, p, "cal: raw=", sizeof(buf));
    p += HtoA(raw, buf + p);
    p = AppendCStr(buf, p, " fmt=", sizeof(buf));
    UtoA((unsigned)g_RBits, b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, '.', sizeof(buf));
    UtoA((unsigned)g_GBits, b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, '.', sizeof(buf));
    UtoA((unsigned)g_BBits, b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);

    g_Calibrated = 1;
}

void GtUnpack16(unsigned short px, unsigned char* r, unsigned char* g,
                unsigned char* b)
{
    if (!g_ExpReady) GtBuildExpand();
    if (g_GBits == 6)
    {
        *r = g_ExpR[(px >> 11) & 0x1Fu];
        *g = g_ExpG[(px >> 5) & 0x3Fu];
        *b = g_ExpB[px & 0x1Fu];
    }
    else
    {
        *r = g_ExpR[(px >> 10) & 0x1Fu];
        *g = g_ExpG[(px >> 5) & 0x1Fu];
        *b = g_ExpB[px & 0x1Fu];
    }
}

// ---------------- GDI rasterisation ----------------
// Slots cache one font each: charge% uses [SWSidebar] FontPx while the hover
// name uses the global [VecText] size; sharing a slot would rebuild the DIB and
// font on every call, which is slower than re-rasterising.

static GdiSlot g_Slots[GDI_SLOTS];
GdiSlot* g_G = &g_Slots[0];

// ---------------- row raster cache (RenderLine reuse) ----------------
#define ROWC_N 64
#define ROWC_W 256
#define ROWC_H 32
#define ROWC_TX 80
typedef struct {
    int gen;
    const void* owner;     /* GdiSlot*: same string in another slot must not alias */
    int len, w, h;
    wchar_t tx[ROWC_TX];
    unsigned char cov[ROWC_W * ROWC_H];
} RowCE;
static RowCE g_RowC[ROWC_N];
static int g_RowGen[GDI_SLOTS] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static int g_RowRR = 0;

static void GdiFreeSlot(GdiSlot* s)
{
    int i;
    if (s->dc)
    {
        SelectObject(s->dc, s->oldBmp);
        DeleteObject(s->dib);
        DeleteObject(s->font);
        DeleteDC(s->dc);
    }
    s->dc = NULL; s->dib = NULL; s->oldBmp = NULL; s->font = NULL;
    s->bits = NULL; s->pitch = 0; s->px = -1; s->bold = -1; s->adv = 0;
    s->face[0] = 0;
    for (i = 0; i < ABC_MEMO_N; i++) { s->abcM[i] = -1; s->abcK[i] = 0; }
    if (s->sidx >= 0 && s->sidx < GDI_SLOTS) g_RowGen[s->sidx]++;
}

int GdiEnsure(int fontPx, int bold, int slot, const wchar_t* face)
{
    HDC screen;
    BITMAPINFO bi;
    TEXTMETRICW tm;
    HFONT f;
    int wantBold = bold ? 1 : 0;
    GdiSlot* s;
    char b1[16], b2[16], buf[256];
    unsigned p;

    if (slot < 0 || slot >= GDI_SLOTS) slot = 0;
    s = &g_Slots[slot];
    s->sidx = slot;

    if (s->dc && fontPx == s->px && wantBold == s->bold && FaceSame(face, s->face)) { g_G = s; return 1; }
    GdiFreeSlot(s);

    screen = GetDC(NULL);
    if (!screen) return 0;
    s->dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!s->dc) return 0;

    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = DIB_W;
    bi.bmiHeader.biHeight = -DIB_H;   /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biSizeImage = 0;
    bi.bmiHeader.biXPelsPerMeter = 0;
    bi.bmiHeader.biYPelsPerMeter = 0;
    bi.bmiHeader.biClrUsed = 0;
    bi.bmiHeader.biClrImportant = 0;

    s->dib = CreateDIBSection(s->dc, &bi, DIB_RGB_COLORS, (void**)&s->bits, NULL, 0);
    if (!s->dib || !s->bits) { GdiFreeSlot(s); return 0; }
    s->oldBmp = (HBITMAP)SelectObject(s->dc, s->dib);
    s->pitch = DIB_W * 4;

    f = CreateFontW(-fontPx, 0, 0, 0, wantBold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (!f) { GdiFreeSlot(s); return 0; }
    s->font = f;
    SelectObject(s->dc, s->font);
    SetBkMode(s->dc, TRANSPARENT);
    SetTextColor(s->dc, RGB(255, 255, 255));

    GetTextMetricsW(s->dc, &tm);
    s->px   = fontPx;
    s->bold = wantBold;
    s->adv  = tm.tmHeight + 2;
    CopyFace(s->face, face);
    g_G = s;

    if (g_LogDetail)
    {
        p = 0;
        p = AppendCStr(buf, p, "gdi: slot=", sizeof(buf));
        UtoA((unsigned)slot, b2);
        p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " fontPx=", sizeof(buf));
        p += UtoA((unsigned)fontPx, buf + p);
        p = AppendCStr(buf, p, " bold=", sizeof(buf));
        UtoA((unsigned)wantBold, b2);
        p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " tmHeight=", sizeof(buf));
        UtoA((unsigned)tm.tmHeight, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " lineAdv=", sizeof(buf));
        UtoA((unsigned)s->adv, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " face=", sizeof(buf));
        { int fi = 0; while (face[fi] && p < sizeof(buf) - 1)
          { wchar_t wc = face[fi++]; buf[p++] = (wc >= 32 && wc < 127) ? (char)wc : '?'; } buf[p] = 0; }
        LogLine(buf);
    }
    return 1;
}

int GtCharABC(wchar_t ch, int* pa, int* pb, int* pc)
{
    GdiSlot* s = g_G;
    ABC abc;
    int idx;
    if (!s || !s->dc || !ch) return -1;
    /* multiplicative hash: consecutive CJK code points would collide modulo. */
    idx = (int)(((((unsigned)ch) * 2654435761u) >> 16) & (ABC_MEMO_N - 1));
    if (s->abcK[idx] != ch)
    {
        if (!GetCharABCWidthsW(s->dc, ch, ch, &abc)) return -1;
        s->abcA[idx] = (short)abc.abcA;
        s->abcB[idx] = (short)abc.abcB;
        s->abcC[idx] = (short)abc.abcC;
        s->abcM[idx] = (short)((abc.abcA + abc.abcB + abc.abcC) & 0x7FFF);
        s->abcK[idx] = ch;
    }
    if (pa) *pa = s->abcA[idx];
    if (pb) *pb = s->abcB[idx];
    if (pc) *pc = s->abcC[idx];
    return s->abcM[idx];
}

int GtCharAdv(wchar_t ch)
{
    return GtCharABC(ch, 0, 0, 0);
}

int CharsFit(const wchar_t* text, int len, int maxW)
{
    int fit = 0;
    SIZE sz;
    if (!g_G->dc || maxW <= 0 || len <= 0) return 0;
    if (!GetTextExtentExPointW(g_G->dc, text, len, maxW, &fit, NULL, &sz)) return 0;
    return fit;
}

static void ClearDibRect(int cols, int rows)
{
    int y, i, w;
    if (cols > DIB_W) cols = DIB_W;
    if (rows > DIB_H) rows = DIB_H;
    if (cols < 0) cols = 0;
    if (rows < 0) rows = 0;
    if (!g_G || !g_G->bits) return;
    w = (cols * 4 + 3) >> 2;
    for (y = 0; y < rows; y++)
    {
        unsigned int* p = (unsigned int*)(g_G->bits + y * g_G->pitch);
        for (i = 0; i < w; i++) p[i] = 0;
    }
}

#define ROW_PAD 2

static void RowCE_Blit(RowCE* e)
{
    int y, x, cw = e->w + ROW_PAD;
    if (cw > ROWC_W) cw = ROWC_W;
    for (y = 0; y < e->h; y++)
    {
        unsigned char* row = g_G->bits + y * g_G->pitch;
        const unsigned char* cv = e->cov + y * ROWC_W;
        for (x = 0; x < cw; x++) row[x * 4 + 2] = cv[x];
    }
}

static int RowCE_Get(const wchar_t* text, int len, RowCE** pe)
{
    int i, k;
    if (len > ROWC_TX) return 0;
    for (i = 0; i < ROWC_N; i++)
    {
        RowCE* e = &g_RowC[i];
        if (e->gen != g_RowGen[g_G->sidx] || e->owner != g_G || e->len != len)
            continue;
        for (k = 0; k < len; k++)
            if (e->tx[k] != text[k]) break;
        if (k == len) { *pe = e; return 1; }
    }
    return 0;
}

static void RowCE_Put(const wchar_t* text, int len, int w, int h)
{
    RowCE* e;
    int y, x, cw;
    cw = w + ROW_PAD;
    if (w <= 0 || cw > ROWC_W || h <= 0 || h > ROWC_H || len > ROWC_TX) return;
    e = &g_RowC[g_RowRR % ROWC_N];
    g_RowRR++;
    e->gen = g_RowGen[g_G->sidx];
    e->owner = g_G;
    e->len = len;
    e->w = w;
    e->h = h;
    for (x = 0; x < len; x++) e->tx[x] = text[x];
    for (y = 0; y < h; y++)
    {
        const unsigned char* row = g_G->bits + y * g_G->pitch;
        unsigned char* cv = e->cov + y * ROWC_W;
        for (x = 0; x < cw; x++) cv[x] = row[x * 4 + 2];
    }
}

int RenderLine(const wchar_t* text, int len)
{
    SIZE sz;
    RowCE* e;
    if (!g_G->dc || len <= 0) return 0;
    {
        int fit = CharsFit(text, len, DIB_W - 8);
        if (fit < 0) fit = 0;
        if (fit < len) len = fit;
        if (len <= 0) return 0;
    }
    if (RowCE_Get(text, len, &e))
    {
        RowCE_Blit(e);
        return e->w;
    }
    if (!GetTextExtentPoint32W(g_G->dc, text, len, &sz)) return 0;
    if (sz.cx > DIB_W - 8) sz.cx = DIB_W - 8;
    if (g_G->adv < 1) ClearDibRect(sz.cx + 8, DIB_H);
    else ClearDibRect(sz.cx + 8, g_G->adv + 8);
    ExtTextOutW(g_G->dc, 0, 0, 0, NULL, text, (UINT)len, NULL);
    RowCE_Put(text, len, sz.cx, g_G->adv);
    return sz.cx;
}

int GtRenderGlyphAt(wchar_t ch, int x0, int y0, int* outW)
{
    SIZE sz;
    if (!g_G || !g_G->dc) return 0;
    if (x0 < 0 || y0 < 0 || x0 > DIB_W - 32) return 0;
    if (!GetTextExtentPoint32W(g_G->dc, &ch, 1, &sz)) return 0;
    ClearDibRect(x0 + sz.cx + 16, DIB_H);
    ExtTextOutW(g_G->dc, x0, y0, 0, NULL, &ch, 1, NULL);
    if (outW) *outW = sz.cx;
    return sz.cx;
}

void BlitCoverage(unsigned char* dstBase, int dstPitch, int surfW, int surfH,
                  int dx, int dy, int w, int h,
                  unsigned fr, unsigned fg, unsigned fb, unsigned alphaScale)
{
    int y, x, x0, x1;
    if (!g_G || !g_G->bits || w <= 0 || h <= 0) return;
    x0 = (dx < 0) ? -dx : 0;
    x1 = (dx + w > surfW) ? surfW - dx : w;
    if (x1 <= x0) return;
    if (alphaScale > 255u) alphaScale = 255u;
    if (fr == 0 && fg == 0 && fb == 0)
    {
        for (y = 0; y < h; y++)
        {
            int sy = dy + y;
            unsigned char* dibRow;
            unsigned short* dstRow;
            if (sy < 0 || sy >= surfH) continue;
            dibRow = g_G->bits + y * g_G->pitch + x0 * 4;
            dstRow = (unsigned short*)(dstBase + sy * dstPitch) + dx + x0;
            if (alphaScale == 100u)
            {
                for (x = x0; x < x1; x++)
                {
                    unsigned cov = dibRow[x * 4 + 2];
                    if (!cov) continue;
                    dstRow[x] = BlendDark(dstRow[x], cov);
                }
            }
            else
            {
                for (x = x0; x < x1; x++)
                {
                    unsigned cov = dibRow[x * 4 + 2];
                    unsigned a;
                    if (!cov) continue;
                    a = (cov * alphaScale + 50u) / 100u;
                    if (a > 255u) a = 255u;
                    dstRow[x] = BlendDark(dstRow[x], a);
                }
            }
        }
        return;
    }
    for (y = 0; y < h; y++)
    {
        int sy = dy + y;
        unsigned char* dibRow;
        unsigned short* dstRow;
        if (sy < 0 || sy >= surfH) continue;
        dibRow = g_G->bits + y * g_G->pitch + x0 * 4;
        dstRow = (unsigned short*)(dstBase + sy * dstPitch) + dx + x0;
        if (alphaScale == 100u)
        {
            for (x = x0; x < x1; x++)
            {
                unsigned cov = dibRow[x * 4 + 2];
                if (!cov) continue;
                dstRow[x] = Blend16(dstRow[x], fr, fg, fb, cov);
            }
        }
        else
        {
            for (x = x0; x < x1; x++)
            {
                unsigned cov = dibRow[x * 4 + 2];
                unsigned a;
                if (!cov) continue;
                a = (cov * alphaScale + 50u) / 100u;
                if (a > 255u) a = 255u;
                dstRow[x] = Blend16(dstRow[x], fr, fg, fb, a);
            }
        }
    }
}

#define SHD_MAXK 16
void BlitCoverageShadow(unsigned char* dstBase, int dstPitch,
                        int surfW, int surfH,
                        int dx, int dy, int sw, int sh,
                        int Size, int offX, int offY,
                        unsigned alphaScale)
{
    const unsigned char* srow[SHD_MAXK];
    unsigned short* drow;
    int scol[SHD_MAXK];
    int X0, X1, Y, X, ox, oy, i, k;
    if (!g_G || !g_G->bits || sw <= 0 || sh <= 0) return;
    if (Size < 1) Size = 1;
    if (Size > 4) Size = 4;
    if (alphaScale > 255u) alphaScale = 255u;
    X0 = dx + offX;
    X1 = dx + offX + Size - 1 + sw - 1;
    if (X1 < X0 || surfW <= 0 || surfH <= 0) return;
    if (X1 < 0) return;
    if (X0 < 0) X0 = 0;
    if (X1 > surfW - 1) X1 = surfW - 1;
    for (Y = dy + offY; Y <= dy + offY + Size - 1 + sh - 1; Y++)
    {
        if (Y < 0 || Y >= surfH) continue;
        drow = (unsigned short*)(dstBase + (size_t)Y * dstPitch);
        k = 0;
        for (oy = 0; oy < Size && k < SHD_MAXK; oy++)
        {
            int sy = Y - dy - offY - oy;
            if (sy < 0 || sy >= sh) continue;
            for (ox = 0; ox < Size && k < SHD_MAXK; ox++)
            {
                srow[k] = g_G->bits + (size_t)sy * g_G->pitch;
                scol[k] = dx + offX + ox;
                k++;
            }
        }
        if (!k) continue;
        for (X = X0; X <= X1; X++)
        {
            unsigned m = 255u;
            int any = 0;
            for (i = 0; i < k; i++)
            {
                int sx = X - scol[i];
                unsigned cov, a;
                if (sx < 0 || sx >= sw) continue;
                cov = srow[i][sx * 4 + 2];
                if (!cov) continue;
                a = (cov * alphaScale + 50u) / 100u;
                if (a > 255u) a = 255u;
                m = (m * (255u - a) + 127u) / 255u;
                any = 1;
            }
            if (!any) continue;
            drow[X] = BlendDark(drow[X], 255u - m);
        }
    }
}
