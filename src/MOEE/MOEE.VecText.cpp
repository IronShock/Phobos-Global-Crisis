// Port of MO_ExperienceEnhancements' GUI/VecText.cpp into Phobos (stage V1).
//
// V1 covers the core: BitFont::Blit glyph takeover (in-game text) and
// GetTextDimension width/height takeover, both via Syringe hooks with a naked
// `ret 0x10` takeover stub and plain `return 0` fall-through. No runtime
// trampolines. See MOEE.VecText.h for the safety notes.
//
// Font sizing: the pixel size is calibrated against the engine's *current* font
// line height and never multiplied by TextScale's global factor. Phobos'
// TextScale swaps the engine font per target surface (in-game text scaled, the
// sidebar / dialogs / shell kept at the original size); by inheriting the line
// height our vector text automatically follows exactly the same scope, so the
// sidebar is never enlarged.
//
// Font selection mirrors the CnCNet client's Resources/Translations/<lang>/
// Fonts.ini chain: a Latin primary plus a CJK fallback, picked per glyph. In
// CjkOnly mode (default) only CJK glyphs are taken over and Latin keeps the
// engine's bitmap font.

#include "MOEE.VecText.h"

#include "MOEE.h"
#include "MOEE.GdiText.h"

#include <Phobos.h>
#include <Utilities/Macro.h>
#include <Misc/TextScale.h>

namespace MOEE::VecText
{
	// ---------------- config / state ----------------
	static bool g_Enabled = false;
	static int  g_FontPx = 0;        // 0 = auto-calibrate; >0 = absolute px
	static bool g_Bold = false;      // selects the Bold TTF pair
	static int  g_DigitTrack = 0;
	static bool g_CjkOnly = true;    // true: only CJK glyphs are taken over
	static bool g_MenuTake = true;    // replace dialog-button text entirely
	static int  g_CreditPx = 0;       // top-strip digit size (0 = follow global)
	static int  g_CreditDY = 0;       // top-strip vertical nudge

	static wchar_t g_FaceCjk[FONT_FACE_MAX] = L"HarmonyOS Sans SC";
	static wchar_t g_FaceCjkBold[FONT_FACE_MAX] = L"HarmonyOS Sans SC";
	static wchar_t g_FaceLatin[FONT_FACE_MAX] = L"Microsoft YaHei";
	static wchar_t g_FaceLatinBold[FONT_FACE_MAX] = L"Microsoft YaHei";

	static bool g_Inited = false;

	unsigned g_BlitTaken = 0;
	unsigned g_DimTaken = 0;

	// BitFont object layout (verified against gamemd.exe).
	#define BF_IDATA   0x04
	#define BF_BUFFER  0x0C
	#define BF_PITCH   0x10
	#define BF_COLOR   0x24
	#define BF_STEP    0x1C
	#define BF_CLIP_T  0x34
	#define BF_CLIP_B  0x3C

	#define IDATA_LINE 0x0C

	void EnsureInit()
	{
		if (g_Inited)
			return;

		g_Inited = true;
		g_Enabled = Phobos::Config::MOEE_VecText;
		g_FontPx = Phobos::Config::MOEE_VecText_FontPx;
		g_Bold = Phobos::Config::MOEE_VecText_Bold;
		g_DigitTrack = Phobos::Config::MOEE_VecText_DigitTrack;
		g_CjkOnly = Phobos::Config::MOEE_VecText_CjkOnly;
		g_MenuTake = Phobos::Config::MOEE_VecText_MenuTake;
		g_CreditPx = Phobos::Config::MOEE_VecText_CreditPx;
		g_CreditDY = Phobos::Config::MOEE_VecText_CreditDY;
		ResolveFontFace(Phobos::Config::MOEE_VecText_FontFace, "vtext", g_FaceCjk, L"HarmonyOS Sans SC");
		ResolveFontFace(Phobos::Config::MOEE_VecText_FontFaceBold, "vtextb", g_FaceCjkBold, L"HarmonyOS Sans SC");
		ResolveFontFace(Phobos::Config::MOEE_VecText_FontFaceLatin, "vtextl", g_FaceLatin, L"Microsoft YaHei");
		ResolveFontFace(Phobos::Config::MOEE_VecText_FontFaceLatinBold, "vtextlb", g_FaceLatinBold, L"Microsoft YaHei");
		Log("vtext", "en=%d font=%d bold=%d cjkOnly=%d (V1)", g_Enabled ? 1 : 0, g_FontPx, g_Bold ? 1 : 0, g_CjkOnly ? 1 : 0);
	}

	static inline int NormC(int v) { return (int)(short)(v & 0xFFFF); }

	// CJK / kana / Hangul / fullwidth ranges use the CJK face; everything else
	// (Latin, digits, ASCII punctuation) uses the Latin face.
	static int IsCjk(unsigned c)
	{
		return (c >= 0x2E80u && c < 0xA000u) ||
		       (c >= 0xAC00u && c < 0xD7A4u) ||
		       (c >= 0xF900u && c < 0xFB00u) ||
		       (c >= 0xFE30u && c < 0xFE50u) ||
		       (c >= 0xFF00u && c < 0xFFF0u) ||
		       (c >= 0x20000u);
	}

	// ---------------- pixel size ----------------
	// A small cache so the scaled (in-game) and unscaled (sidebar / dialog / shell)
	// font variants used by TextScale each get their own GDI slot instead of
	// rebuilding one slot twice per frame.
	#define VC_MAXW 48
	#define VC_MAXH 48
	#define VC_FAIL (-1000000)
	#define PX_N 4

	// Advance-width only (glyph bitmap byte 0), no ink scan. Cheaper for GetDim's
	// per-character width loop.
	static int EngineGlyphWidth(void* idata, wchar_t ch)
	{
		unsigned char* p = (unsigned char*)idata;
		const unsigned short* gsym;
		unsigned char* bitmaps;
		int stride, gi, gcount;

		if (!idata || !IsPlausiblePtr(idata))
			return 0;

		stride = *(int*)(p + 0x14);
		gsym = *(const unsigned short**)(p + 0x18);
		bitmaps = *(unsigned char**)(p + 0x1C);
		gcount = *(int*)(p + 0x10);

		if (stride < 1 || stride > 512 || gcount < 1 || gcount > 0x10000)
			return 0;
		if (!gsym || !IsPlausiblePtr(gsym) || !bitmaps || !IsPlausiblePtr(bitmaps))
			return 0;

		gi = gsym[(unsigned)ch & 0xFFFFu];
		if (!gi || gi > gcount)
			return 0;

		return bitmaps[(size_t)stride * (gi - 1)];
	}

	typedef struct { int used; void* idata; int bold; int src; int px; } PxEnt;
	static PxEnt g_Px[PX_N];
	static int g_PxRR = 0;

	static int ScanInk(int px, wchar_t ch, int idx, int* pT, int* pB);

	// Measured ink height (and width) of one glyph in the engine's bitmap font.
	// InternalData: BytesPerRow(+4), Rows(+8), LineHeight(+0xC), GlyphStride(+0x14),
	// SymbolTable(+0x18), Bitmaps(+0x1C). Each glyph bitmap is `GlyphStride` bytes:
	// byte 0 = advance width, then `Rows` rows of `BytesPerRow` bytes (1bpp).
	static int EngineGlyphInk(void* idata, wchar_t ch, int* pAdv)
	{
		unsigned char* p = (unsigned char*)idata;
		const unsigned short* gsym;
		unsigned char* bitmaps;
		unsigned char* g;
		int stride, bpr, rows, gi, gcount, j, k, any, T, B;

		if (!idata || !IsPlausiblePtr(idata))
			return 0;

		bpr = *(int*)(p + 0x04);
		rows = *(int*)(p + 0x08);
		stride = *(int*)(p + 0x14);
		gsym = *(const unsigned short**)(p + 0x18);
		bitmaps = *(unsigned char**)(p + 0x1C);
		gcount = *(int*)(p + 0x10);

		if (bpr < 1 || bpr > 16 || rows < 4 || rows > 64 || stride < 1 || stride > 512)
			return 0;
		if (gcount < 1 || gcount > 0x10000)
			return 0;
		if (!gsym || !IsPlausiblePtr(gsym) || !bitmaps || !IsPlausiblePtr(bitmaps))
			return 0;

		gi = gsym[(unsigned)ch & 0xFFFFu];
		if (!gi || gi > gcount)
			return 0;

		// The glyph pointer is a byte offset inside the (4-byte aligned) bitmap
		// block, so it is usually NOT 4-byte aligned - do not use IsPlausiblePtr
		// on it, only a bounds check against GlyphCount.
		g = bitmaps + (size_t)stride * (gi - 1);

		if (pAdv)
			*pAdv = g[0];

		T = rows; B = -1;
		for (j = 0; j < rows; j++)
		{
			unsigned char* row = g + 1 + (size_t)j * bpr;
			any = 0;
			for (k = 0; k < bpr; k++)
				if (row[k]) { any = 1; break; }
			if (any) { if (j < T) T = j; if (j > B) B = j; }
		}

		if (B < T)
			return 0;

		return B - T + 1;
	}

	// Returns the pixel size for this font object and, via *pIdx, the cache slot
	// index (used to pick a distinct GDI slot).
	static int PxFor(void* t, int* pIdx)
	{
		void* idata;
		int i, px, origH, T, B, inkH, bold = g_Bold ? 1 : 0;

		if (!t || !IsPlausiblePtr(t))
			return -1;

		idata = *(void**)((char*)t + BF_IDATA);
		if (!idata || !IsPlausiblePtr(idata))
			return -1;

		for (i = 0; i < PX_N; i++)
		{
			if (g_Px[i].used && g_Px[i].idata == idata && g_Px[i].bold == bold && g_Px[i].src == g_FontPx)
			{
				if (pIdx) *pIdx = i;
				return g_Px[i].px;
			}
		}

		origH = *(int*)((char*)idata + IDATA_LINE);
		if (origH < 8 || origH > 40)
			return -1;

		if (g_FontPx > 0)
		{
			px = g_FontPx;
		}
		else
		{
			// Match the GDI glyph ink to the engine's own glyph ink height (measured
			// from the bitmap font data) so the vector text is the same visual size
			// as the engine's and lines do not overlap. Falls back to the engine
			// line height if the glyph cannot be measured.
			int px0 = 18;
			int gw = 0;
			int target = EngineGlyphInk(idata, 0x6C38, &gw);   // 永
			if (target < 4 || target > 64)
				target = origH;   // fallback: line height (MO_EE behaviour)

			T = DIB_H; B = -1;
			if (!ScanInk(px0, 0x6C38, 0, &T, &B))
				return -1;

			inkH = B - T + 1;
			if (inkH < 1)
				return -1;

			px = (px0 * target + inkH / 2) / inkH;

			{
				static bool s_calLogged = false;
				if (!s_calLogged)
				{
					s_calLogged = true;
					Log("vtext", "cal lineH=%d inkTarget=%d gw=%d ink18=%d px=%d", origH, target, gw, inkH, px);
				}
			}
		}

		if (px < 8) px = 8;
		if (px > 40) px = 40;

		// Insert (reuse the round-robin slot).
		if (g_PxRR >= PX_N) g_PxRR = 0;
		i = g_PxRR++;
		g_Px[i].used = 1; g_Px[i].idata = idata; g_Px[i].bold = bold; g_Px[i].src = g_FontPx; g_Px[i].px = px;
		if (pIdx) *pIdx = i;
		return px;
	}

	// ---------------- GDI slots ----------------
	// idx 0..PX_N-1 -> CJK slots 2..5, Latin slots 6..9.
	static int EnsureForChar(int px, wchar_t wc, int idx)
	{
		int cjk = IsCjk((unsigned)wc);

		if (idx < 0 || idx >= PX_N) idx = 0;

		if (cjk)
			return GdiEnsure(px, 0, 2 + idx, g_Bold ? g_FaceCjkBold : g_FaceCjk);

		return GdiEnsure(px, 0, 6 + idx, g_Bold ? g_FaceLatinBold : g_FaceLatin);
	}

	// Ink bounding box of one glyph rendered at (8,8) into the shared DIB.
	static int ScanInk(int px, wchar_t ch, int idx, int* pT, int* pB)
	{
		SIZE sz;
		int gw, x, y, scanW;
		unsigned char* bits;
		int pitch;

		if (!EnsureForChar(px, ch, idx))
			return 0;

		if (GtRenderGlyphAt(ch, 8, 8, NULL) <= 0)
			return 0;

		gw = 0;
		if (g_G && g_G->dc && GetTextExtentPoint32W(g_G->dc, &ch, 1, &sz))
			gw = sz.cx;

		scanW = 8 + gw + 8;
		if (scanW > DIB_W) scanW = DIB_W;
		bits = g_G->bits;
		pitch = g_G->pitch;

		for (y = 0; y < DIB_H; y++)
		{
			unsigned char* row = bits + y * pitch;
			for (x = 0; x < scanW; x++)
				if (row[x * 4 + 2]) { if (y < *pT) *pT = y; if (y > *pB) *pB = y; }
		}

		return 1;
	}

	// ---------------- glyph cache ----------------
	typedef struct {
		int used;
		wchar_t wch;
		void* idata;
		int px, adv, h, idx;
		int outh, off, fullW;
		unsigned char rowLo[VC_MAXH], rowHi[VC_MAXH];
		unsigned char cov[VC_MAXW * VC_MAXH];
	} VCEnt;

	#define VC_MASK 2047
	static VCEnt g_VC[VC_MASK + 1];

	// Shared vertical window per GDI slot so all glyphs of a face keep a baseline.
	static int g_RefValid[10];
	static int g_RefPx[10];
	static int g_RefBold[10];
	static int g_RefT[10], g_RefB[10];

	static void RefCalibrate(int px, int idx)
	{
		int slot = 2 + idx;
		int T, B;

		if (slot < 0 || slot > 9) slot = 2;

		if (g_RefValid[slot] && g_RefPx[slot] == px && g_RefBold[slot] == (g_Bold ? 1 : 0))
			return;

		T = DIB_H; B = -1;
		ScanInk(px, 0x6C38, idx, &T, &B);   // 永 (CJK)
		ScanInk(px, 0x4E00, idx, &T, &B);   // 一 (CJK)

		if (B < T) { T = 8; B = 8 + px - 1; }

		g_RefValid[slot] = 1; g_RefPx[slot] = px; g_RefBold[slot] = g_Bold ? 1 : 0;
		g_RefT[slot] = T; g_RefB[slot] = B;
	}

	static VCEnt* Glyph(void* t, wchar_t wch, int adv, int h, int px, int idx)
	{
		unsigned gi;
		VCEnt* e;
		void* idata;
		int gw, dx, y, L, R, inkW, x, off, gdiW, scanW;
		unsigned char* bits;
		int pitch;
		int outH, slot;

		if (!t || !IsPlausiblePtr(t) || px < 8 || px > 40)
			return NULL;

		gi = ((unsigned)wch) & VC_MASK;
		e = &g_VC[gi];
		idata = *(void**)((char*)t + BF_IDATA);

		if (e->used && e->wch == wch && e->idata == idata && e->px == px &&
			e->adv == adv && e->h == h && e->idx == idx)
			return e;

		if (!EnsureForChar(px, wch, idx))
			return NULL;

		RefCalibrate(px, idx);

		// Re-ensure: RefCalibrate probes other cells and leaves g_G elsewhere.
		if (!EnsureForChar(px, wch, idx))
			return NULL;

		gw = GtRenderGlyphAt(wch, 8, 8, NULL);
		if (gw <= 0 || h > VC_MAXH)
			return NULL;

		bits = g_G->bits;
		pitch = g_G->pitch;
		scanW = 8 + gw + 8;
		if (scanW > DIB_W) scanW = DIB_W;

		slot = 2 + idx;
		if (slot < 0 || slot > 9) slot = 2;
		outH = g_RefB[slot] - g_RefT[slot] + 1;
		if (outH < 1) outH = h;
		if (outH > VC_MAXH) return NULL;
		if (g_RefT[slot] < 0 || g_RefT[slot] + outH > DIB_H) return NULL;

		L = scanW; R = -1;
		for (y = 0; y < outH; y++)
		{
			unsigned char* row = bits + (g_RefT[slot] + y) * pitch;
			for (x = 0; x < scanW; x++)
				if (row[x * 4 + 2]) { if (x < L) L = x; if (x > R) R = x; }
		}

		inkW = (R >= L) ? R - L + 1 : 0;

		{
			int a = 0, b = 0, c = 0;
			gdiW = GtCharABC(wch, &a, &b, &c);
			if (gdiW < 0) gdiW = 0;
			off = a;
			if (off < 0) off = 0;
		}

		if (off + inkW > VC_MAXW) return NULL;

		for (y = 0; y < outH; y++)
		{
			unsigned char* row = bits + (g_RefT[slot] + y) * pitch;
			unsigned char* dst = e->cov + y * VC_MAXW;
			int lo = VC_MAXW, hi = -1;

			for (dx = 0; dx < VC_MAXW; dx++) dst[dx] = 0;

			if (!inkW) { e->rowLo[y] = 1; e->rowHi[y] = 0; continue; }

			for (dx = 0; dx < inkW; dx++)
			{
				unsigned char a2 = row[(L + dx) * 4 + 2];
				dst[off + dx] = a2;
				if (a2) { if (off + dx < lo) lo = off + dx; if (off + dx > hi) hi = off + dx; }
			}

			e->rowLo[y] = (unsigned char)(hi >= lo ? lo : 1);
			e->rowHi[y] = (unsigned char)(hi >= lo ? hi : 0);
		}

		e->used = 1; e->wch = wch; e->idata = idata; e->px = px; e->adv = adv; e->h = h; e->idx = idx;
		e->outh = outH; e->off = off; e->fullW = off + inkW;
		return e;
	}

	// Advance for one character in its own face (+ digit tracking).
	static int CharAdv(int px, wchar_t wc, int idx)
	{
		int adv;

		if (!EnsureForChar(px, wc, idx))
			return -1;

		adv = GtCharAdv(wc);
		if (adv < 0)
			return -1;

		if (wc >= 0x30 && wc <= 0x39)
			adv += g_DigitTrack;

		if (adv < 0)
			adv = 0;

		return adv;
	}

	// ---------------- menu button takeover ----------------
	// A dialog button's text is drawn by one DrawText call with a known call site
	// and box. We capture the whole string there and re-lay it out ourselves in
	// the Blit hook (centred in the button), keeping the engine's per-character
	// bookkeeping untouched.
	#define MENU_MAXCH 64
	#define MV_MASK 127
	typedef struct {
		int used; wchar_t wch; void* idata; int px, adv, h, off, inkW, outh, gdiW;
		unsigned char cov[VC_MAXW * VC_MAXH];
	} MVEnt;
	static MVEnt g_MV[MV_MASK + 1];

	static wchar_t g_MenuStr[MENU_MAXCH + 1];
	static int g_MenuN = 0, g_MenuRX = 0, g_MenuRW = 0, g_MenuIdx = 0, g_MenuBuilt = 0, g_MenuActive = 0, g_MenuCjk = 0;
	static int g_MenuAdv[MENU_MAXCH + 1];
	static int g_MenuPenX = 0;

	static int MenuMatch(unsigned caller, int w, int h, int flags)
	{
		if (!g_MenuTake) return 0;
		if (caller != 0x00621144u) return 0;
		if (flags != 5) return 0;
		if ((w == 123 && h == 24) || (w == 121 && h == 20)) return 1;
		return 0;
	}

	static MVEnt* MenuGlyph(void* t, wchar_t wch, int adv, int h, int px, int idx)
	{
		unsigned gi;
		MVEnt* e;
		void* idata;
		int gw, dx, y, L, R, inkW, x, off, gdiW, scanW, outH, slot;
		unsigned char* bits;
		int pitch;

		if (!t || !IsPlausiblePtr(t) || px < 8 || px > 40)
			return NULL;

		gi = ((unsigned)wch) & MV_MASK;
		e = &g_MV[gi];
		idata = *(void**)((char*)t + BF_IDATA);

		if (e->used && e->wch == wch && e->idata == idata && e->px == px &&
			e->adv == adv && e->h == h)
			return e;

		if (!EnsureForChar(px, wch, idx))
			return NULL;

		RefCalibrate(px, idx);

		if (!EnsureForChar(px, wch, idx))
			return NULL;

		gw = GtRenderGlyphAt(wch, 8, 8, NULL);
		if (gw <= 0 || h > VC_MAXH)
			return NULL;

		bits = g_G->bits;
		pitch = g_G->pitch;
		scanW = 8 + gw + 8;
		if (scanW > DIB_W) scanW = DIB_W;

		slot = 2 + idx;
		if (slot < 0 || slot > 9) slot = 2;
		outH = g_RefB[slot] - g_RefT[slot] + 1;
		if (outH < 1) outH = h;
		if (outH > VC_MAXH) return NULL;
		if (g_RefT[slot] < 0 || g_RefT[slot] + outH > DIB_H) return NULL;

		L = scanW; R = -1;
		for (y = 0; y < outH; y++)
		{
			unsigned char* row = bits + (g_RefT[slot] + y) * pitch;
			for (x = 0; x < scanW; x++)
				if (row[x * 4 + 2]) { if (x < L) L = x; if (x > R) R = x; }
		}

		inkW = (R >= L) ? R - L + 1 : 0;

		{
			int a = 0, b = 0, c = 0;
			gdiW = GtCharABC(wch, &a, &b, &c);
			if (gdiW < 0) gdiW = 0;
			off = a;
			if (off < 0) off = 0;
		}

		if (off + inkW > VC_MAXW) return NULL;

		for (y = 0; y < outH; y++)
		{
			unsigned char* row = bits + (g_RefT[slot] + y) * pitch;
			unsigned char* dst = e->cov + y * VC_MAXW;

			for (dx = 0; dx < VC_MAXW; dx++) dst[dx] = 0;
			if (!inkW) continue;

			for (dx = 0; dx < inkW; dx++)
				dst[off + dx] = row[(L + dx) * 4 + 2];
		}

		e->used = 1; e->wch = wch; e->idata = idata; e->px = px; e->adv = adv; e->h = h;
		e->off = off; e->inkW = inkW; e->outh = outH; e->gdiW = gdiW;
		return e;
	}

	static int MenuStep(MVEnt* me, int fallback)
	{
		int st;

		if (!me) return fallback;
		if (me->gdiW <= 0)
			return (me->inkW > 0) ? me->off + me->inkW : fallback;

		st = me->gdiW;
		if (me->wch >= 0x30 && me->wch <= 0x39) st += g_DigitTrack;
		if (st < 1) st = 1;
		return st;
	}

	static int MenuRender(void* t, wchar_t wch, int adv, int h, int x, int y,
	                      int colorW, int px, int idx)
	{
		unsigned short* buf;
		int pitchW, Tp, B, dx, dy, sy2;
		unsigned char fr, fg, fb;
		MVEnt* e;

		if (!t || !IsPlausiblePtr(t)) return -1;
		if (adv > VC_MAXW || h > VC_MAXH) return -1;

		buf = *(unsigned short**)((char*)t + BF_BUFFER);
		pitchW = *(int*)((char*)t + BF_PITCH);
		if (!buf || !IsPlausiblePtr(buf) || pitchW < 64 || pitchW > 8192) return -1;

		Tp = NormC(*(int*)((char*)t + BF_CLIP_T));
		B = NormC(*(int*)((char*)t + BF_CLIP_B));
		if (B < Tp) return -1;
		if (Tp < 0) Tp = 0;
		if (B > 8192) B = 8192;

		if (!g_Calibrated) Calibrate();

		e = MenuGlyph(t, wch, adv, h, px, idx);
		if (!e) return -1;

		GtUnpack16((unsigned short)(colorW & 0xFFFF), &fr, &fg, &fb);

		dy = y + (h - e->outh) / 2;
		{
			int lo = Tp, hi = B - e->outh + 1;
			if (hi >= lo) { if (dy < lo) dy = lo; if (dy > hi) dy = hi; }
		}

		for (sy2 = 0; sy2 < e->outh; sy2++)
		{
			int sy = dy + sy2;
			unsigned short* row;
			unsigned char* cov;

			if (sy < Tp || sy > B) continue;

			row = buf + sy * pitchW;
			cov = e->cov + sy2 * VC_MAXW;

			for (dx = 0; dx < e->inkW; dx++)
			{
				int sx = x + e->off + dx;
				unsigned a;

				if (sx < 0 || sx >= pitchW) continue;
				a = cov[e->off + dx];
				if (!a) continue;
				row[sx] = Blend16(row[sx], fr, fg, fb, a);
			}
		}

		return x + adv;
	}

	// ---------------- campaign briefing body self-draw ----------------
	// The engine lays the briefing out with its own bitmap metrics (line pitch,
	// advance and wrapping are tied together), so the whole block is redrawn:
	// line pitch follows BriefLine, tracking follows BriefTrack, wrapping by ink
	// width. The engine call still runs (it binds the surface and colour); the
	// Blit hook grabs the first character and suppresses the rest.
	#define BRIEF_MAXCH 4096
	#define BRIEF_PAD 20
	#define BRIEF_MARK_RGB 0x40

	static int g_Brief = 1;
	static int g_BriefLine = 125;
	static int g_BriefOwn = 1;
	static int g_BriefTrack = 0;
	static int g_BriefMark = 1;
	static int g_BriefWidth = 580;
	static int g_BriefAdv = 1;
	static int g_FontOrigH = 19;

	static wchar_t* g_BriefStr = NULL;
	static int g_BriefX = 0, g_BriefY = 0, g_BriefW = 0, g_BriefH = 0;
	static int g_BriefState = 0;
	static int g_DTBody = 0;
	static int g_BriefCfg = 0;

	static void BriefEnsureCfg(void)
	{
		if (g_BriefCfg) return;
		g_BriefCfg = 1;
		g_Brief = Phobos::Config::MOEE_VecText_Brief ? 1 : 0;
		g_BriefLine = Phobos::Config::MOEE_VecText_BriefLine;
		g_BriefOwn = Phobos::Config::MOEE_VecText_BriefOwn ? 1 : 0;
		g_BriefTrack = Phobos::Config::MOEE_VecText_BriefTrack;
		g_BriefMark = Phobos::Config::MOEE_VecText_BriefMark ? 1 : 0;
		g_BriefWidth = Phobos::Config::MOEE_VecText_BriefWidth;
		g_BriefAdv = Phobos::Config::MOEE_VecText_BriefAdv ? 1 : 0;
	}

	static int VecIsBriefCaller(unsigned caller)
	{
		if (caller >= 0x00479000u && caller < 0x00479100u) return 1;
		if (caller == 0x00621144u) return 1;
		return 0;
	}

	static int BriefLineH(int px)
	{
		int h = (g_FontOrigH > 0) ? g_FontOrigH : px;

		if (g_BriefLine > 0)
		{
			int v = (px * g_BriefLine + 50) / 100;
			if (v > h) h = v;
		}

		if (h > VC_MAXH) h = VC_MAXH;
		if (h < 8) h = 8;
		return h;
	}

	static int BriefMaxY(void)
	{
		int h = GtGameH();
		if (h < 64 || h > 8192) return -1;
		return h - 1;
	}

	static int BriefCellW(int px, wchar_t ch, int tr, int idx)
	{
		int ga;

		if (!EnsureForChar(px, ch, idx))
			return px + tr;

		ga = GtCharAdv(ch);
		if (ga <= 0 || ga > 64)
			ga = px;
		if (ga < 1)
			ga = 1;
		return ga + tr;
	}

	static int BriefDrawChar(void* t, wchar_t wc, int adv, int h, int x, int y,
	                         int colorW, int px, int idx)
	{
		unsigned short* buf;
		int pitchW, Tp, B, dx, dy, sy2;
		unsigned char fr, fg, fb;
		VCEnt* e;

		buf = *(unsigned short**)((char*)t + BF_BUFFER);
		pitchW = *(int*)((char*)t + BF_PITCH);
		if (!buf || !IsPlausiblePtr(buf) || pitchW < 1024 || pitchW > 8192)
			return -1;

		Tp = NormC(*(int*)((char*)t + BF_CLIP_T));
		B = NormC(*(int*)((char*)t + BF_CLIP_B));
		if (B < Tp) return -1;
		if (Tp < 0) Tp = 0;
		if (B > 8192) B = 8192;

		if (!g_Calibrated) Calibrate();

		e = Glyph(t, wc, adv, h, px, idx);
		if (!e) return -1;

		GtUnpack16((unsigned short)(colorW & 0xFFFF), &fr, &fg, &fb);

		dy = y + (h - e->outh) / 2;
		{
			int lo = Tp, hi = B - e->outh + 1;
			if (hi >= lo) { if (dy < lo) dy = lo; if (dy > hi) dy = hi; }
		}

		for (sy2 = 0; sy2 < e->outh; sy2++)
		{
			int sy = dy + sy2;
			int lo2, hi2, x0, x1;
			unsigned short* row;
			unsigned char* cov;

			if (sy < Tp || sy > B) continue;

			lo2 = e->rowLo[sy2];
			hi2 = e->rowHi[sy2];
			if (hi2 < lo2) continue;

			row = buf + sy * pitchW;
			cov = e->cov + sy2 * VC_MAXW;

			x0 = lo2;
			x1 = hi2;
			if (x + x0 < 0) x0 = -x;
			if (x + x1 >= pitchW) x1 = pitchW - 1 - x;

			for (dx = x0; dx <= x1; dx++)
			{
				unsigned a = cov[dx];
				if (!a) continue;
				row[x + dx] = Blend16(row[x + dx], fr, fg, fb, a);
			}
		}

		return x + adv;
	}

	static void BriefRect(unsigned short* buf, int pitchW, const int* LTRB,
	                      int x0, int y0, int x1, int y1, int thick)
	{
		unsigned short* row;
		int lef = LTRB[0], top = LTRB[1], rig = LTRB[2], bot = LTRB[3];
		int i, t, sx, sy;

		if (x1 < x0) { i = x0; x0 = x1; x1 = i; }
		if (y1 < y0) { i = y0; y0 = y1; y1 = i; }
		if (lef < 0) lef = 0;
		if (top < 0) top = 0;
		if (rig > pitchW - 1) rig = pitchW - 1;

		for (i = 0; i < 2; i++)
		{
			sy = i ? y1 : y0;
			row = buf + sy * pitchW;
			for (t = 0; t < thick; t++, sy++)
			{
				if (sy < top || sy > bot) continue;
				for (sx = x0; sx <= x1; sx++)
					if (sx >= lef && sx <= rig)
						row[sx] = Blend16(row[sx], BRIEF_MARK_RGB, BRIEF_MARK_RGB, BRIEF_MARK_RGB, 255);
			}
		}

		for (i = 0; i < 2; i++)
		{
			sx = i ? x1 : x0;
			for (t = 0; t < thick; t++, sx++)
			{
				if (sx < lef || sx > rig) continue;
				for (sy = y0; sy <= y1; sy++)
					if (sy >= top && sy <= bot)
						buf[sy * pitchW + sx] = Blend16(buf[sy * pitchW + sx],
							BRIEF_MARK_RGB, BRIEF_MARK_RGB, BRIEF_MARK_RGB, 255);
			}
		}
	}

	static void BriefMark(unsigned short* buf, int pitchW, const int* LTRB,
	                      int x0, int y0, int x1, int y1)
	{
		int d = 2 + 2;
		BriefRect(buf, pitchW, LTRB, x0, y0, x1, y1, 2);
		if (x1 - d <= x0 + d || y1 - d <= y0 + d) return;
		BriefRect(buf, pitchW, LTRB, x0 + d, y0 + d, x1 - d, y1 - d, 1);
	}

	// Returns 0 = taken over, -1 = leave it to the engine.
	static int BriefBody(void* font, const wchar_t* s, int x, int y, int w)
	{
		int px, lh, tr, adv, color, cx, cy, lines, seg, per, want, oldB, n, oa;
		int pitchW, maxCx, maxY, oldL, oldT, oldR;
		int* LTRB;
		const wchar_t* p;
		int idx = 0;

		BriefEnsureCfg();
		if (!g_Brief) return -1;

		if (!font || !IsPlausiblePtr(font)) return -1;
		if (!s || !IsPlausiblePtr((void*)s)) return -1;
		if (!*(unsigned short**)((char*)font + BF_BUFFER)) return -1;

		{
			void* bidata = *(void**)((char*)font + BF_IDATA);
			if (bidata && IsPlausiblePtr(bidata))
			{
				int oh = *(int*)((char*)bidata + IDATA_LINE);
				if (oh >= 8 && oh <= 40) g_FontOrigH = oh;
			}
		}

		pitchW = *(int*)((char*)font + BF_PITCH);
		if (pitchW < 1024) return -1;
		if (w < 64) return -1;
		if (g_BriefWidth > 0 && g_BriefWidth < 4096) w = g_BriefWidth;

		px = PxFor(font, &idx);
		if (px < 8 || px > 40) return -1;

		lh = BriefLineH(px);
		tr = g_BriefTrack;
		if (tr < 0) tr = 0;
		if (tr > VC_MAXW - px) tr = VC_MAXW - px;
		adv = px + tr;

		color = (int)(*(unsigned short*)((char*)font + BF_COLOR));
		maxY = BriefMaxY();
		if (maxY < 0) return -1;

		per = w / adv;
		if (per < 1) return -1;

		lines = 1;
		cx = x;
		for (p = s; *p && lines < BRIEF_MAXCH; p++)
		{
			if (*p == 0x0A) { cx = x; lines++; continue; }
			seg = BriefCellW(px, *p, tr, idx);
			if (cx + seg > x + w) { cx = x; lines++; }
			cx += seg;
		}

		LTRB = (int*)((char*)font + 0x30);
		oldL = LTRB[0]; oldT = LTRB[1]; oldR = LTRB[2]; oldB = LTRB[3];

		if (LTRB[0] > x - BRIEF_PAD - 4) LTRB[0] = (x - BRIEF_PAD - 4 > 0) ? x - BRIEF_PAD - 4 : 0;
		if (LTRB[1] > y - BRIEF_PAD - 4) LTRB[1] = (y - BRIEF_PAD - 4 > 0) ? y - BRIEF_PAD - 4 : 0;
		if (LTRB[2] < x + w + BRIEF_PAD + 4) LTRB[2] = (x + w + BRIEF_PAD + 4 < pitchW) ? x + w + BRIEF_PAD + 4 : pitchW - 1;
		want = y + lines * lh + BRIEF_PAD + 4;
		if (want > 1000) want = 1000;
		if (want > maxY) want = maxY;
		if (LTRB[3] < want) LTRB[3] = want;

		maxCx = x;

		for (cx = x, cy = y, p = s; *p; p++)
		{
			int cw;

			if (*p == 0x0A) { cx = x; cy += lh; continue; }

			oa = 0;
			if (!EnsureForChar(px, *p, idx)) { cw = adv; }
			else
			{
				oa = GtCharAdv(*p);
				cw = (g_BriefAdv && oa > 0) ? BriefCellW(px, *p, tr, idx) : adv;
			}

			if (cw > VC_MAXW) cw = VC_MAXW;
			if (cx + cw > x + w) { cx = x; cy += lh; }
			if (cy + lh > y + lines * lh + lh) break;

			n = BriefDrawChar(font, *p, cw - tr, lh, cx, cy, color, px, idx);
			if (n < 0) { cx += cw; continue; }
			cx = n + tr;
			if (cx > maxCx) maxCx = cx;
		}

		if (g_BriefMark)
			BriefMark(*(unsigned short**)((char*)font + BF_BUFFER), pitchW, LTRB,
			          x - BRIEF_PAD, y - BRIEF_PAD, maxCx + BRIEF_PAD, cy + lh + BRIEF_PAD);

		LTRB[0] = oldL; LTRB[1] = oldT; LTRB[2] = oldR; LTRB[3] = oldB;

		{
			static bool s_logged = false;
			if (!s_logged)
			{
				s_logged = true;
				MOEE::Log("vtext", "brief: own draw box=%d,%d %d lh=%d lines=%d px/char=%d pitch=%d",
					x, y, w, lh, lines, adv, pitchW);
			}
		}

		return 0;
	}
}

// ---------------- takeover plumbing ----------------
extern "C" __declspec(naked) void MOEE_VecRet16()
{
	__asm { ret 0x10 }
}

// BitFont::Blit(this, wch, x, y, color)
DEFINE_HOOK(0x434120, MOEE_VecText_Blit, 0x6)
{
	using namespace MOEE::VecText;

	static bool s_logged = false;
	if (!s_logged) { s_logged = true; MOEE::Log("vtext", "Blit hook live"); }

	EnsureInit();
	if (!g_Enabled)
		return 0;

	// Keep TextScale's per-call bookkeeping (font swap + message-list row pitch)
	// running even though we short-circuit its own Blit hook below.
	TextScale::Update();

	void* t = (void*)R->ECX();
	int wch = R->Stack<int>(0x4);
	int x = R->Stack<int>(0x8);
	int y = R->Stack<int>(0xC);
	int color = R->Stack<int>(0x10);

	if (!t || !IsPlausiblePtr(t))
		return 0;

	wchar_t wc = (wchar_t)((unsigned)wch & 0xFFFFu);
	if (wc == 9)
		return 0;

	int idx = 0;
	int px = PxFor(t, &idx);
	if (px < 8 || px > 40)
		return 0;

	// Top-strip digits (y < 32) can use their own size / vertical nudge. Only
	// meaningful when Latin is vectorised (i.e. CjkOnly is off).
	int topDigit = (!g_CjkOnly && wc >= 0x30 && wc <= 0x39 && y >= -8 && y < 32);

	if (topDigit && g_CreditPx > 0)
	{
		px = g_CreditPx;
		if (px < 8) px = 8;
		if (px > 40) px = 40;
	}

	int adv = CharAdv(px, wc, idx);
	if (adv < 0)
		return 0;
	if (adv < 1)
		adv = 1;

	x = NormC(x);
	y = NormC(y);

	int h = *(int*)((char*)t + BF_STEP);
	if (h < 8 || h > 64)
		h = px;

	unsigned short* buf = *(unsigned short**)((char*)t + BF_BUFFER);
	int pitchW = *(int*)((char*)t + BF_PITCH);
	if (!buf || !IsPlausiblePtr(buf) || pitchW < 64 || pitchW > 8192)
		return 0;

	int Tp = NormC(*(int*)((char*)t + BF_CLIP_T));
	int B = NormC(*(int*)((char*)t + BF_CLIP_B));
	if (B < Tp)
	{
		int gh = GtGameH();
		if (gh < 64)
			return 0;
		Tp = 0; B = gh - 1;
	}
	if (Tp < 0) Tp = 0;
	if (B > 8192) B = 8192;

	if (!g_Calibrated)
		Calibrate();

	int eff = (color == -1) ? (int)(*(unsigned short*)((char*)t + BF_COLOR)) : color;

	// ---- campaign briefing body ----
	if (g_DTBody && g_BriefState != 2)
	{
		if (g_BriefState == 1)
		{
			R->EAX(x + adv);
			return (DWORD)(size_t)&MOEE_VecRet16;
		}

		g_BriefState = 1;

		if (BriefBody(t, g_BriefStr, g_BriefX, g_BriefY, g_BriefW) == 0)
		{
			R->EAX(x + adv);
			return (DWORD)(size_t)&MOEE_VecRet16;
		}

		g_BriefState = 2;   // unusable: let the engine draw from here on
	}

	// ---- menu button takeover (dialog buttons) ----
	if (g_MenuActive && g_MenuN > 0 && (!g_CjkOnly || g_MenuCjk))
	{
		if (!g_MenuBuilt)
		{
			int mi, tot = 0, ok = 1;

			for (mi = 0; mi < g_MenuN; mi++)
			{
				int ca = CharAdv(px, g_MenuStr[mi], idx);
				MVEnt* mg;

				if (ca < 0) ca = 8;
				g_MenuAdv[mi] = ca;
				mg = MenuGlyph(t, g_MenuStr[mi], ca, h, px, idx);

				if (!mg) { ok = 0; break; }
				tot += MenuStep(mg, ca);
			}

			if (ok)
			{
				g_MenuPenX = g_MenuRX + (g_MenuRW - tot) / 2;
				if (g_MenuPenX < g_MenuRX) g_MenuPenX = g_MenuRX;
				g_MenuBuilt = 1;
			}
		}

		if (g_MenuBuilt && g_MenuIdx < g_MenuN && wc == g_MenuStr[g_MenuIdx])
		{
			int rr = MenuRender(t, wc, g_MenuAdv[g_MenuIdx], h, g_MenuPenX, y, eff, px, idx);

			if (rr >= 0)
			{
				MVEnt* me = MenuGlyph(t, wc, g_MenuAdv[g_MenuIdx], h, px, idx);
				int step = MenuStep(me, g_MenuAdv[g_MenuIdx]);

				if (step < 1) step = 1;
				g_MenuPenX += step;
				g_MenuIdx++;
				if (g_MenuIdx >= g_MenuN) g_MenuActive = 0;

				R->EAX(x + adv);
				return (DWORD)(size_t)&MOEE_VecRet16;
			}
		}

		return 0;   // not built / mismatch -> let the engine draw this character
	}

	if (g_CjkOnly && !IsCjk((unsigned)wc))
		return 0;

	VCEnt* e = Glyph(t, wc, adv, h, px, idx);
	if (!e)
		return 0;

	unsigned char fr, fg, fb;
	GtUnpack16((unsigned short)(eff & 0xFFFF), &fr, &fg, &fb);

	int dy = y + (topDigit ? g_CreditDY : 0) + (h - e->outh) / 2;
	{
		int lo = Tp, hi = B - e->outh + 1;
		if (hi >= lo)
		{
			if (dy < lo) dy = lo;
			if (dy > hi) dy = hi;
		}
	}

	int sy2;
	for (sy2 = 0; sy2 < e->outh; sy2++)
	{
		int sy = dy + sy2;
		int lo, hi, dx, x0, x1;
		unsigned short* row;
		unsigned char* cov;

		if (sy < Tp || sy > B)
			continue;

		lo = e->rowLo[sy2];
		hi = e->rowHi[sy2];
		if (hi < lo)
			continue;

		row = buf + sy * pitchW;
		cov = e->cov + sy2 * VC_MAXW;

		x0 = lo;
		x1 = hi;
		if (x + x0 < 0) x0 = -x;
		if (x + x1 >= pitchW) x1 = pitchW - 1 - x;
		if (x1 < x0)
			continue;

		for (dx = x0; dx <= x1; dx++)
		{
			unsigned a = cov[dx];
			if (!a)
				continue;
			row[x + dx] = Blend16(row[x + dx], fr, fg, fb, a);
		}
	}

	R->EAX(x + adv);
	{
		static bool s_first = false;
		static unsigned s_last = 0;

		if (!s_first) { s_first = true; MOEE::Log("vtext", "first Blit takeover wc=0x%X px=%d adv=%d", (unsigned)wc, px, adv); }
		g_BlitTaken++;

		unsigned now = GetTickCount();
		if (s_last == 0) s_last = now;
		else if (now - s_last >= 10000u)
		{
			s_last = now;
			MOEE::Log("vtext", "10s blit=%u dim=%u", g_BlitTaken, g_DimTaken);
		}
	}
	return (DWORD)(size_t)&MOEE_VecRet16;
}

// bool GetTextDimension(this, const wchar_t* s, int* w, int* h, int maxW)
DEFINE_HOOK(0x433CF0, MOEE_VecText_GetDim, 0x6)
{
	using namespace MOEE::VecText;

	EnsureInit();
	if (!g_Enabled)
		return 0;

	// Keep TextScale's per-call bookkeeping (font swap + message-list row pitch)
	// running even though we short-circuit its own GetDim hook below.
	TextScale::Update();

	void* t = (void*)R->ECX();
	const wchar_t* s = R->Stack<const wchar_t*>(0x4);
	int* w = R->Stack<int*>(0x8);
	int* h = R->Stack<int*>(0xC);
	int maxW = R->Stack<int>(0x10);

	if (!t || !IsPlausiblePtr(t) || !s || !IsPlausiblePtr((void*)s) || !w)
		return 0;

	void* idata = *(void**)((char*)t + BF_IDATA);
	if (!idata || !IsPlausiblePtr(idata))
		return 0;

	int lineH = *(int*)((char*)idata + IDATA_LINE);
	if (lineH < 8 || lineH > 40)
		return 0;

	int idx = 0;
	int px = PxFor(t, &idx);
	if (px < 8 || px > 40)
		return 0;

	// Layout: greedy wrap that prefers breaking at a space (word wrap) and only
	// falls back to breaking mid-run when there is no space on the line (CJK has
	// none, so CJK behaves as a per-character wrap). Advances: our own for CJK,
	// the engine's bitmap glyph width for the rest.
	//
	// The engine's GetTextDimension returns only ONE line height for wrapped
	// text, which makes the message background box too short (text spills out)
	// once the font is scaled. Returning the true line count fixes the box.
	int i, lineW = 0, maxLine = 0, lines = 1, any = 0;
	int lastSpaceW = -1, lastSpaceEndW = -1;

	for (i = 0; i < 256; i++)
	{
		wchar_t c = s[i];
		int ga;

		if (!c)
			break;

		if (c < 0x20)
			return 0;

		if (g_CjkOnly && !IsCjk((unsigned)c))
		{
			ga = EngineGlyphWidth(idata, c);
			if (ga <= 0)
				return 0;                 // unknown glyph -> let the engine do it
			ga += 1;                      // glyph width + inter-char space
		}
		else
		{
			ga = CharAdv(px, c, idx);
			if (ga < 0)
				return 0;
		}

		if (ga < 0)
			ga = 0;

		any = 1;

		if (maxW > 0 && lineW > 0 && lineW + ga > maxW)
		{
			if (lastSpaceW >= 0)
			{
				// Break at the last space: the line ends before it, and the word
				// after the space starts the next line.
				if (lastSpaceW > maxLine) maxLine = lastSpaceW;
				lineW -= lastSpaceEndW;
			}
			else
			{
				if (lineW > maxLine) maxLine = lineW;
				lineW = 0;
			}

			lines++;
			lastSpaceW = -1;
			lastSpaceEndW = -1;

			if (lines > 64)
				return 0;
		}

		lineW += ga;

		if (c == L' ')
		{
			lastSpaceW = lineW - ga;   // width before the space
			lastSpaceEndW = lineW;     // width after the space
		}
	}

	if (i >= 256 || !any)
		return 0;

	if (lastSpaceW > maxLine) maxLine = lastSpaceW;
	if (lineW > maxLine) maxLine = lineW;

	*w = maxLine;
	if (h)
		*h = lines * lineH;

	{
		static bool s_first = false;

		if (!s_first) { s_first = true; MOEE::Log("vtext", "first GetDim takeover n=%d lines=%d w=%d h=%d", i, lines, maxLine, lines * lineH); }
		g_DimTaken++;
	}

	R->EAX(1);
	return (DWORD)(size_t)&MOEE_VecRet16;
}

// The remaining three entry points are observed only (pure pass-through) in V1.

DEFINE_HOOK(0x434B90, MOEE_VecText_Print, 0x5)
{
	using namespace MOEE::VecText;

	static bool s_logged = false;
	if (!s_logged) { s_logged = true; MOEE::Log("vtext", "Print hook live"); }

	g_MenuActive = 0;   // a menu capture is only valid inside its own DrawText
	g_DTBody = 0;       // likewise for the briefing body
	return 0;
}

DEFINE_HOOK(0x434CD0, MOEE_VecText_DrawText, 0x5)
{
	using namespace MOEE::VecText;

	static bool s_logged = false;
	if (!s_logged) { s_logged = true; MOEE::Log("vtext", "DrawText hook live"); }

	EnsureInit();

	if (g_Enabled)
	{
		TextScale::Update();

		unsigned caller = (unsigned)R->Stack<DWORD>(0);
		const wchar_t* str = R->Stack<const wchar_t*>(0xC);
		int X = R->Stack<int>(0x10);
		int Y = R->Stack<int>(0x14);
		int W = R->Stack<int>(0x18);
		int H = R->Stack<int>(0x1C);
		int flags = R->Stack<int>(0x20);

		g_MenuActive = 0;
		g_DTBody = 0;

		// Dialog button text: capture the whole string and re-lay it out in Blit.
		if (g_MenuTake && MenuMatch(caller, W, H, flags))
		{
			int mn = 0, allCjk = 1;

			if (str && IsPlausiblePtr(str))
			{
				while (mn < MENU_MAXCH && str[mn] && str[mn] != 0x0A)
				{
					if (!IsCjk((unsigned)str[mn])) allCjk = 0;
					g_MenuStr[mn] = str[mn];
					mn++;
				}
			}

			g_MenuStr[mn] = 0;
			g_MenuN = mn;
			g_MenuRX = X;
			g_MenuRW = W;
			g_MenuIdx = 0;
			g_MenuBuilt = 0;
			g_MenuCjk = allCjk;
			g_MenuActive = (mn > 0) ? 1 : 0;
		}

		// Campaign briefing body: one DrawText call with a big box; the Blit hook
		// grabs its first character and redraws the whole block.
		BriefEnsureCfg();

		if (g_Brief && VecIsBriefCaller(caller) && caller == 0x00621144u && W >= 400 && H >= 100)
		{
			g_BriefStr = (wchar_t*)str;
			g_BriefX = X;
			g_BriefY = Y;
			g_BriefW = W;
			g_BriefH = H;
			g_BriefState = 0;
			g_DTBody = 1;
		}
	}

	return 0;
}

DEFINE_HOOK(0x4A5EB0, MOEE_VecText_Simple, 0x7)
{
	static bool s_logged = false;
	if (!s_logged) { s_logged = true; MOEE::Log("vtext", "Simple hook live"); }
	return 0;
}
