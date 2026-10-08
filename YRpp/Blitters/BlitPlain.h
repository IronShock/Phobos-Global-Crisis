#pragma once

#include "Blitter.h"

DEFINE_BLITTER(BlitPlain)
{
public:
	inline explicit BlitPlain() noexcept
	{
	}

	virtual ~BlitPlain() override final = default;

	virtual void Blit_Copy(void* dst, byte * src, int len, int zval, WORD * zbuf, WORD * abuf, int alvl, int warp) override final
	{
		for (int i = 0; i < len; ++i)
			reinterpret_cast<T*>(dst)[i] = reinterpret_cast<T*>(src)[i];
	}

	virtual void Blit_Copy_Tinted(void* dst, byte * src, int len, int zval, WORD * zbuf, WORD * abuf, int alvl, int warp, WORD tint)
	{
		Blit_Copy(dst, src, len, zval, zbuf, abuf, alvl, 0);
	}

	virtual void Blit_Move(void* dst, byte * src, int len, int zval, WORD * zbuf, WORD * abuf, int alvl, int warp)
	{
		Blit_Copy(dst, src, len, zval, zbuf, abuf, alvl, 0);
	}

	virtual void Blit_Move_Tinted(void* dst, byte * src, int len, int zval, WORD * zbuf, WORD * abuf, int alvl, int warp, WORD tint)
	{
		Blit_Copy(dst, src, len, zval, zbuf, abuf, alvl, 0);
	}
};
