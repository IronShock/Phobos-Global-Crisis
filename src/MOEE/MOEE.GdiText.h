#pragma once

// Port of MO_ExperienceEnhancements' GUI/GdiText.h into Phobos.
//
// Shared base for the MOEE GUI modules (chat box now, vector text later):
// no-CRT string helpers, GDI glyph rasterisation into a DIB, the 16-bit blend
// pipeline, the surface (DSURFACE) vtable wrappers and the font resolver.
//
// Differences from upstream: the custom RectangleStruct/Point2D/ColorStruct
// typedefs are dropped (the YRpp ones are used), the MOExperience.log file
// logger is replaced by Phobos' Debug log, and the no-CRT memcpy/memset/
// memmove overrides are gone (this build links the CRT).

#include <Windows.h>
#include <GeneralStructures.h>

// ---------------- small string helpers (no CRT dependency kept) ----------------

unsigned UtoA(unsigned v, char* p);
unsigned HtoA(unsigned v, char* p);
unsigned AppendCStr(char* dst, unsigned pos, const char* src, unsigned cap);
unsigned AppendCh(char* dst, unsigned pos, char c, unsigned cap);
bool IsPlausiblePtr(const void* p);

// ---------------- logging ----------------
// Forwarded to Phobos' debug log with a [MOEE:gdi] prefix.
void LogLine(const char* body);
extern int g_LogDetail; // 0 = key events only (default), 1 = verbose

// ---------------- mouse / window (kept for the other GUI modules) ----------------
int GtMouseGameXY(int* x, int* y);            // 1 = valid
void GtRefreshWinSize(unsigned tick);

// ---------------- surface (DSURFACE) vtable slots ----------------
#define GtVT_FILL_RECT        0x14
#define GtVT_FILL_RECT_TRANS  0x1C
#define GtVT_GET_PIXEL        0x28
#define GtVT_LOCK             0x5C
#define GtVT_UNLOCK           0x60
#define GtVT_GET_BPP          0x70
#define GtVT_GET_PITCH        0x74
#define GtVT_GET_WIDTH        0x7C
#define GtVT_GET_HEIGHT       0x80

#define ADDR_SURF_COMPOSITE   0x0088731Cu
#define ADDR_SURF_SIDEBAR     0x00887300u
#define ADDR_SURF_PRIMARY     0x00887308u
#define ADDR_SURF_TEMP        0x00887314u
#define ADDR_SURF_VIEWBOUNDS  0x00886FA0u
#define ADDR_BITFONT_INSTANCE 0x0089C4D0u
#define FONT_OFF_HEIGHT       0x1C

int GtSurfGet(void* surf, unsigned vtOff);    // negative on failure
void GtFillRectTrans(void* surf, RectangleStruct* r, ColorStruct* c, int opacity);
unsigned char* GtLock(void* surf);            // NULL on failure; pair with GtUnlock
void GtUnlock(void* surf);
void GtDrawOutline(void* surf, int x, int y, int w, int h,
                   unsigned char r, unsigned char g, unsigned char b, int op);
int GtBitLineHeight(void);                    // bitmap font line height (fallback 14)
int GtGameW(void);
int GtGameH(void);
void GtSetGameSize(int w, int h);
int GtScreenW(void);                          // ViewBounds.Width (fallback 640)
int GtGameFocused(void);                      // 1 = game window is foreground

// ---------------- engine clock ----------------
unsigned GtTickNow(void);
unsigned GtClockNow(void);

// ---------------- font resolution ----------------
// Config holds a font *file name* (e.g. simhei.ttf), matched verbatim.
// Order: game folder -> system font folder -> fallback family.
#define FONT_FACE_MAX 48
void CopyFace(wchar_t* dst, const wchar_t* src);
int FaceSame(const wchar_t* a, const wchar_t* b);
int FileExists(const char* path);
int FindFontFile(const char* name, char* outPath, int cap, const char** tag);
void ResolveFontFace(const char* cfg, const char* tag, wchar_t* out,
                     const wchar_t* fallback);

// ---------------- pixel format ----------------
extern int g_RBits, g_GBits, g_BBits;
extern int g_Calibrated;
void Calibrate(void);      // idempotent: fills the Temp surface red and reads back
extern unsigned char g_ExpR[32], g_ExpG[64], g_ExpB[32];
extern int g_ExpReady;
void GtBuildExpand(void);

// 16-bit blend: dst = fg*a + dst*(1-a), fg is 8-bit, a = 0..255.
static __inline unsigned short Blend16(unsigned short dst, unsigned fr,
                                       unsigned fg, unsigned fb, unsigned a)
{
    unsigned dr8, dg8, db8, or_, og, ob;
    if (a == 0) return dst;
    if (!g_ExpReady) GtBuildExpand();
    if (a == 255)
    {
        if (g_GBits == 6)
            return (unsigned short)((((fr * 31u + 127u) / 255u) << 11) |
                                    (((fg * 63u + 127u) / 255u) << 5) |
                                     ((fb * 31u + 127u) / 255u));
        return (unsigned short)((((fr * 31u + 127u) / 255u) << 10) |
                                (((fg * 31u + 127u) / 255u) << 5) |
                                 ((fb * 31u + 127u) / 255u));
    }
    if (g_GBits == 6)
    {
        dr8 = g_ExpR[(dst >> 11) & 0x1Fu];
        dg8 = g_ExpG[(dst >> 5) & 0x3Fu];
        db8 = g_ExpB[dst & 0x1Fu];
        or_ = (fr * a + dr8 * (255u - a) + 127u) / 255u;
        og = (fg * a + dg8 * (255u - a) + 127u) / 255u;
        ob = (fb * a + db8 * (255u - a) + 127u) / 255u;
        or_ = (or_ * 31u + 127u) / 255u;
        og = (og * 63u + 127u) / 255u;
        ob = (ob * 31u + 127u) / 255u;
        return (unsigned short)((or_ << 11) | (og << 5) | ob);
    }
    dr8 = g_ExpR[(dst >> 10) & 0x1Fu];
    dg8 = g_ExpG[(dst >> 5) & 0x1Fu];
    db8 = g_ExpB[dst & 0x1Fu];
    or_ = (fr * a + dr8 * (255u - a) + 127u) / 255u;
    og = (fg * a + dg8 * (255u - a) + 127u) / 255u;
    ob = (fb * a + db8 * (255u - a) + 127u) / 255u;
    or_ = (or_ * 31u + 127u) / 255u;
    og = (og * 31u + 127u) / 255u;
    ob = (ob * 31u + 127u) / 255u;
    return (unsigned short)((or_ << 10) | (og << 5) | ob);
}

// Pure-black overlay (shadow): same as Blend16(...,0,0,0,a), cheaper.
static __inline unsigned short BlendDark(unsigned short dst, unsigned a)
{
    unsigned m = 255u - a, or_, og, ob_;
    if (a == 0) return dst;
    if (a == 255) return 0;
    if (!g_ExpReady) GtBuildExpand();
    if (g_GBits == 6)
    {
        or_  = (g_ExpR[(dst >> 11) & 0x1Fu] * m + 127u) / 255u;
        og   = (g_ExpG[(dst >> 5) & 0x3Fu] * m + 127u) / 255u;
        ob_  = (g_ExpB[dst & 0x1Fu] * m + 127u) / 255u;
        or_  = (or_ * 31u + 127u) / 255u;
        og   = (og * 63u + 127u) / 255u;
        ob_  = (ob_ * 31u + 127u) / 255u;
        return (unsigned short)((or_ << 11) | (og << 5) | ob_);
    }
    or_ = (g_ExpR[(dst >> 10) & 0x1Fu] * m + 127u) / 255u;
    og  = (g_ExpG[(dst >> 5) & 0x1Fu] * m + 127u) / 255u;
    ob_ = (g_ExpB[dst & 0x1Fu] * m + 127u) / 255u;
    or_ = (or_ * 31u + 127u) / 255u;
    og  = (og * 31u + 127u) / 255u;
    ob_ = (ob_ * 31u + 127u) / 255u;
    return (unsigned short)((or_ << 10) | (og << 5) | ob_);
}

void GtUnpack16(unsigned short px, unsigned char* r, unsigned char* g,
                unsigned char* b);

// ---------------- GDI rasterisation ----------------
#define DIB_W 1024
#define DIB_H 64
#define ABC_MEMO_N 1024

// Slots: 0=battle box 1=hint box 2=global text 3=SW panel/power 4=chat input
// 5=SW charge% 6=SW hover name 7=zoom scale 8=status/top text 9=FPS pin.
#define GDI_SLOTS 10

typedef struct {
    HDC dc;
    HBITMAP dib;
    HBITMAP oldBmp;
    HFONT font;
    unsigned char* bits;
    int pitch;
    int px;     // -1 = empty slot
    int bold;
    int adv;
    int sidx;                     // slot index, invalidates the row cache
    wchar_t face[FONT_FACE_MAX];  // cache key
    // Per-glyph ABC memo; cleared together with the slot.
    short abcM[ABC_MEMO_N];
    short abcA[ABC_MEMO_N], abcB[ABC_MEMO_N], abcC[ABC_MEMO_N];
    wchar_t abcK[ABC_MEMO_N];
} GdiSlot;

extern GdiSlot* g_G;   // current slot
int GdiEnsure(int fontPx, int bold, int slot, const wchar_t* face);
int GtCharAdv(wchar_t ch);                       // ABC total width; -1 on failure
int GtCharABC(wchar_t ch, int* pa, int* pb, int* pc);
int RenderLine(const wchar_t* text, int len);    // rasterise a line into the DIB
int CharsFit(const wchar_t* text, int len, int maxW);
int GtRenderGlyphAt(wchar_t ch, int x0, int y0, int* outW);

// Blit the DIB coverage into a locked 16-bit surface.
void BlitCoverage(unsigned char* dstBase, int dstPitch, int surfW, int surfH,
                  int dx, int dy, int w, int h,
                  unsigned fr, unsigned fg, unsigned fb, unsigned alphaScale);

// Single-pass shadow composition (Size x Size offset union, one blend/pixel).
void BlitCoverageShadow(unsigned char* dstBase, int dstPitch,
                        int surfW, int surfH,
                        int dx, int dy, int sw, int sh,
                        int Size, int offX, int offY,
                        unsigned alphaScale);
