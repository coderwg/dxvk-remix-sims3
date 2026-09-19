/*
 * Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
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

#include "d3d9_util.h"
#include "base.h"
#include "d3d9_device_base.h"
#include "d3d9_commonshader.h"
#include "sims3_camera_hook.h"
#include "sims3_walls.h"

class Direct3DVertexShader9_LSS: public D3DBase<IDirect3DVertexShader9> {
  void onDestroy() override;
  CommonShader m_shader;
protected:
  BaseDirect3DDevice9Ex_LSS* const m_pDevice = nullptr;
public:
  Direct3DVertexShader9_LSS(BaseDirect3DDevice9Ex_LSS* const pDevice, const CommonShader& shader)
    : D3DBase((IDirect3DVertexShader9*) nullptr, pDevice)
    , m_pDevice(pDevice)
    , m_shader(shader) {
  }

  // The Sims 3 camera hook: constant-patch rule for this shader (by bytecode hash), or null;
  // and whether its draws are never captured (1), or not when alpha-blended (2): neverCaptureMode.
  const sims3cam::ShaderPatch* sims3Patch = nullptr;
  uint8_t sims3NeverCapture = 0;
  bool sims3SkyDome = false;    // its draws are presented to the runtime as the sky (isSkyDomeShader)
  uint64_t sims3Hash = 0;     // FNV-1a-64 of the original bytecode (diagnostics)
  sims3cam::VsNormalInfo sims3Normal;   // where the world-space normal leaves this shader (milestone 11)
  sims3cam::WallVsInfo sims3Wall;       // a wall shader's clamp of the up-ness flag, for the opening cut (milestone 13)

  /*** IUnknown methods ***/
  STDMETHOD(QueryInterface)(THIS_ REFIID riid, void** ppvObj);
  STDMETHOD_(ULONG, AddRef)(THIS);
  STDMETHOD_(ULONG, Release)(THIS);

  /*** IDirect3DVertexShader9 methods ***/
  STDMETHOD(GetDevice)(THIS_ IDirect3DDevice9** ppDevice);
  STDMETHOD(GetFunction)(THIS_ void*, UINT* pSizeOfData);

  const CommonShader& getCommonShader() const {
    return m_shader;
  }
};