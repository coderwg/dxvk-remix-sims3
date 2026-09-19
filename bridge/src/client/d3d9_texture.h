/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#pragma once

#include "d3d9_base_texture.h"
#include "d3d9_util.h"

#include <unknwn.h>
#include <d3d9.h>

/*
 * IDirect3DTexture9 LSS Interceptor Class
 */
class Direct3DTexture9_LSS: public LssBaseTexture2D<IDirect3DTexture9> {
  void onDestroy() override;

public:
  Direct3DTexture9_LSS(BaseDirect3DDevice9Ex_LSS* const pDevice, const TEXTURE_DESC& desc)
    : Direct3DBaseTexture9_LSS(pDevice, desc) {
    m_children.resize(GetLevelCount());
  }

  D3DSURFACE_DESC getLevelDesc(const UINT level) const;

  // The Sims 3 camera hook (milestone 13): the client's copy of level 0 (null until the game has
  // locked it) and how many times the game has written it.
  const uint8_t* sims3Level0Data() const;
  uint32_t sims3Level0Version() const;
  // The Sims 3 camera hook: XXH3 of level 0 as the runtime hashes it, from the client's copy of
  // the texel data (0 when the client holds none); cached per upload (sims3Level0Version).
  uint64_t sims3Level0Hash();
  uint64_t sims3Hash = 0; uint32_t sims3HashVersion = 0xFFFFFFFFu;

  /*** IUnknown methods ***/
  STDMETHOD(QueryInterface)(THIS_ REFIID riid, void** ppvObj);
  STDMETHOD_(ULONG, AddRef)(THIS);
  STDMETHOD_(ULONG, Release)(THIS);

  STDMETHOD_(D3DRESOURCETYPE, GetType)(THIS) {
    return D3DRTYPE_TEXTURE;
  }

  /*** IDirect3DTexture9 methods ***/
  STDMETHOD(GetLevelDesc)(THIS_ UINT Level, D3DSURFACE_DESC* pDesc);
  STDMETHOD(GetSurfaceLevel)(THIS_ UINT Level, IDirect3DSurface9** ppSurfaceLevel);
  STDMETHOD(LockRect)(THIS_ UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags);
  STDMETHOD(UnlockRect)(THIS_ UINT Level);
  STDMETHOD(AddDirtyRect)(THIS_ CONST RECT* pDirtyRect);
};