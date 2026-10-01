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

class Direct3DPixelShader9_LSS: public D3DBase<IDirect3DPixelShader9> {
  void onDestroy() override;
  CommonShader m_shader;
protected:
  BaseDirect3DDevice9Ex_LSS* const m_pDevice = nullptr;
public:
  Direct3DPixelShader9_LSS(BaseDirect3DDevice9Ex_LSS* const pDevice, const CommonShader& shader)
    : D3DBase((IDirect3DPixelShader9*) nullptr, pDevice)
    , m_pDevice(pDevice)
    , m_shader(shader) {
  }

  // The Sims 3 camera hook: this pixel shader carries the per-object light rig (by bytecode hash), or null;
  // and the texture stage that is its diffuse/albedo, or -1 when unknown.
  int sims3AlbedoStage = -1;
  int sims3TintReg = -1;      // pixel constant register holding the Create-A-Style tint, or -1
  uint64_t sims3Hash = 0;     // FNV-1a-64 of the bytecode (diagnostics)
  uint8_t sims3Major = 0;     // the bytecode's major version (milestone 56: fixed-function fog leaves 3.0 alone)
  sims3cam::PsAnalysis sims3Auto;   // what the bytecode says about its samplers (untabled shaders: the albedo is chosen from this at draw time)

  /*** IUnknown methods ***/
  STDMETHOD(QueryInterface)(THIS_ REFIID riid, void** ppvObj);
  STDMETHOD_(ULONG, AddRef)(THIS);
  STDMETHOD_(ULONG, Release)(THIS);

  /*** IDirect3DPixelShader9 methods ***/
  STDMETHOD(GetDevice)(THIS_ IDirect3DDevice9** ppDevice);
  STDMETHOD(GetFunction)(THIS_ void*, UINT* pSizeOfData);
};