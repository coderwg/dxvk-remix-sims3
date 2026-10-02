/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
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
#include "pch.h"
#include "d3d9_device.h"
#include "d3d9_lss.h"
#include "d3d9_util.h"
#include "d3d9_surfacebuffer_helper.h"
#include "d3d9_cubetexture.h"
#include "d3d9_indexbuffer.h"
#include "d3d9_pixelshader.h"
#include "d3d9_query.h"
#include "d3d9_surface.h"
#include "d3d9_swapchain.h"
#include "d3d9_texture.h"
#include "sims3_camera_hook.h"
#include "sims3_walls.h"
#include "sims3_lots.h"
#include <atomic>
#include <thread>
// The Sims 3 camera hook (milestone 17): the runtime identifies textures by the XXH3 of their
// level-0 bytes; with xxhash.h next to this file the hook computes its marker textures' hashes
// and sets the runtime's terrain options itself, otherwise the markers are tagged in the menu.
#if __has_include("xxhash.h")
#define XXH_INLINE_ALL
#include "xxhash.h"
#define SIMS3_HAVE_XXHASH 1
#else
#define SIMS3_HAVE_XXHASH 0
#endif

namespace {
  // The Sims 3 camera hook, by subject: these files are parts of this one, included in this
  // anonymous namespace in this order (and three more after the buffer headers below).
#include "sims3_device_state.inl"     // the hook's state and its own buffers
#include "sims3_device_capture.inl"   // a draw's capture: transforms, albedo stage, samplers
#include "sims3_device_terrain.inl"   // the terrain: markers, shader variants, terrain draws
#include "sims3_device_masks.inl"     // the game's state put back, the masked writes
#include "sims3_device_walls.inl"     // the wall openings (milestone 13)
#include "sims3_device_draw.inl"      // the device reset, sims3BeginDraw and sims3EndDraw
}

// Around every draw of the game (the hook's own restore quad is left alone). A reflection pass's
// draw returns here, never sent (nothing was changed on the device for it).
#define SIMS3_BEGIN_DRAW() \
  if (sims3cam::enabled() && !g_sims3.ourDraw) { sims3BeginDraw(g_sims3, this, m_state.renderStates.data(), m_state.streamFreqs[0]); if (g_sims3.drawDropped) return D3D_OK; }
#define SIMS3_END_DRAW() if (sims3cam::enabled() && !g_sims3.ourDraw) sims3EndDraw(g_sims3, this)
#include "d3d9_vertexbuffer.h"
#include "d3d9_vertexdeclaration.h"
#include "d3d9_vertexshader.h"
#include "d3d9_volumetexture.h"
#include "shadow_map.h"
#include "client_options.h"
#include "swapchain_map.h"
#include "config/global_options.h"
#include "remix_api.h"
#include "window.h"

#include "util_bridge_assert.h"
#include "util_semaphore.h"

#include <wingdi.h>
#include <assert.h>

#define GET_PRES_PARAM() (m_pSwapchain->getPresentationParameters())

namespace {
#include "sims3_device_squares.inl"   // the town ground's squares (milestone 60)
#include "sims3_device_lots.inl"      // the low-detail lots (milestones 69-71)
#include "sims3_device_notes.inl"     // the facts about the bound objects (milestone 17w)
}

#define SetShaderConst(func, StartRegister, pConstantData, Count, size, currentUID) \
  { \
    ClientMessage c(Commands::IDirect3DDevice9Ex_##func, getId()); \
    currentUID = c.get_uid(); \
    c.send_many(StartRegister, Count); \
    c.send_data(size, (void*)pConstantData); \
  }

extern NamedSemaphore* gpPresent;
extern std::mutex gSwapChainMapMutex;
extern SwapChainMap gSwapChainMap;

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::onDestroy() {
  // At this point the underlying d3d9 device's refcount should be 0 and device released
  assert(getRef<D3DRefCounted::Ref::Object>() == 0 &&
         "Destroying an LSS device object with underlying D3D9 object refcount > 0!");
   ClientMessage c { Commands::IDirect3DDevice9Ex_Destroy, getId() };
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::releaseInternalObjects(bool resetState) {
  // Take references first so that the device won't be
  // destroyed unintentionally and to prevent releaseInternalObjects()
  // recursion.
  const auto implicitRefCnt = m_implicitRefCnt; // m_implicitRefCnt will invalidate on destroy
  for (uint32_t n = 0; n < implicitRefCnt; n++) {
    D3DBase::AddRef();
  }

  // The Sims 3 camera hook: its own objects (shader variants, scratch targets, cut wall geometry)
  // and references go first, on Reset and on the device's destruction alike.
  if (sims3cam::enabled()) sims3OnReset(g_sims3);

  destroyImplicitObjects();

  if (resetState) {
    for (auto& texture : m_state.textures) {
      texture.reset(nullptr);
    }

    for (auto& rt : m_state.renderTargets) {
      rt.reset(nullptr);
    }

    for (auto& st : m_state.streams) {
      st.reset(nullptr);
    }

    m_state.indices.reset(nullptr);
    m_state.depthStencil.reset(nullptr);
    m_state.vertexShader.reset(nullptr);
    m_state.pixelShader.reset(nullptr);
    m_state.vertexDecl.reset(nullptr);
  }

  for (uint32_t n = 0; n < implicitRefCnt; n++) {
    D3DBase::Release();
  }
}

/*
 * Direct3DDevice9 Interface Implementation
 */
template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::QueryInterface(REFIID riid, LPVOID* ppvObj) {
  ZoneScoped;
  LogFunctionCall();
  if (ppvObj == nullptr) {
    return E_POINTER;
  }

  *ppvObj = nullptr;

  if (riid == __uuidof(IUnknown)
    || riid == __uuidof(IDirect3DDevice9)
    || (m_ex && riid == __uuidof(IDirect3DDevice9Ex))) {
    *ppvObj = bridge_cast<IDirect3DDevice9Ex*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

template<bool EnableSync>
ULONG Direct3DDevice9Ex_LSS<EnableSync>::AddRef() {
  ZoneScoped;
  LogFunctionCall();
  // Let the server control it's own device lifetime completely - no push
  return D3DBase::AddRef() - m_implicitRefCnt;
}

template<bool EnableSync>
ULONG Direct3DDevice9Ex_LSS<EnableSync>::Release() {
  ZoneScoped;
  LogFunctionCall();

  const ULONG cnt = D3DBase::Release();
  const bool bDestroy = !m_bIsDestroying && (cnt == m_implicitRefCnt);
  if (bDestroy) {
    m_bIsDestroying = true;
    // Device is about to be destroyed - release internal objects.
    releaseInternalObjects();
    return 0;
  }

  if (cnt > m_implicitRefCnt) {
    return cnt - m_implicitRefCnt;
  } else {
    return 0;
  }
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::TestCooperativeLevel() {
  ZoneScoped;
  LogFunctionCall();
  // This returns failure on uniqueness change - so ignore it for now, seems benign.
  // TODO: Return device removed when server dies
  return D3D_OK;
}

template<bool EnableSync>
UINT Direct3DDevice9Ex_LSS<EnableSync>::GetAvailableTextureMem() {
  ZoneScoped;
  LogFunctionCall();
  
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetAvailableTextureMem, getId());
    currentUID = c.get_uid();
  }
  WAIT_FOR_SERVER_RESPONSE("GetAvailableTextureMem()", 0, currentUID);
  // Available memory in MB
  UINT mem = (UINT) DeviceBridge::get_data();
  DeviceBridge::pop_front();

  return mem;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::EvictManagedResources() {
  ZoneScoped;
  LogFunctionCall();

  UID currentUID = 0;
  // Send command to server and wait for response
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_EvictManagedResources, getId());
    currentUID = c.get_uid();
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("EvictManagedResources()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetDirect3D(IDirect3D9** ppD3D9) {
  ZoneScoped;
  LogFunctionCall();

  if (ppD3D9 == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      (*ppD3D9) = m_pDirect3D;
      m_pDirect3D->AddRef();
    }
    if (GlobalOptions::getSendReadOnlyCalls()) {
      ClientMessage { Commands::IDirect3DDevice9Ex_GetDirect3D, getId() };
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::internalGetDeviceCaps(D3DCAPS9* pCaps) {
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetDeviceCaps, getId());
    currentUID = c.get_uid();
  }

  WAIT_FOR_SERVER_RESPONSE("GetDeviceCaps()", D3DERR_INVALIDCALL, currentUID);

  HRESULT hresult = DeviceBridge::get_data();
  if (SUCCEEDED(hresult)) {
    uint32_t len = DeviceBridge::copy_data(*pCaps);
    if (len != sizeof(D3DCAPS9) && len != 0) {
      Logger::err("GetDeviceCaps() failed due to issue with data returned from server.");
      hresult = D3DERR_INVALIDCALL;
    }
  }
  DeviceBridge::pop_front();

  return hresult;
}


template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetDeviceCaps(D3DCAPS9* pCaps) {
  ZoneScoped;
  LogFunctionCall();

  if (pCaps == NULL)
    return D3DERR_INVALIDCALL;

  *pCaps = m_caps;

  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) {
  ZoneScoped;
  LogFunctionCall();

  if (pMode == NULL)
    return D3DERR_INVALIDCALL;

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetDisplayMode, getId());
    currentUID = c.get_uid();
    c.send_data(iSwapChain);
  }
  WAIT_FOR_SERVER_RESPONSE("GetDisplayMode()", D3DERR_INVALIDCALL, currentUID);

  HRESULT hresult = DeviceBridge::get_data();
  if (SUCCEEDED(hresult)) {
    uint32_t len = DeviceBridge::copy_data(*pMode);
    if (len != sizeof(D3DDISPLAYMODE) && len != 0) {
      Logger::err("GetDisplayMode() failed due to issue with data returned from server.");
      hresult = D3DERR_INVALIDCALL;
    }
  }
  DeviceBridge::pop_front();
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) {
  ZoneScoped;
  LogFunctionCall();

  if (pParameters == NULL)
    return D3DERR_INVALIDCALL;

  *pParameters = m_createParams;
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) {
  ZoneScoped;
  LogFunctionCall();

  if (pCursorBitmap == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto pLssSurface = bridge_cast<Direct3DSurface9_LSS*>(pCursorBitmap);
  if (pLssSurface) {
    UID currentUID = 0;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetCursorProperties, getId());
      currentUID = c.get_uid();
      c.send_many(XHotSpot, YHotSpot, pLssSurface->getId());
    }
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetCursorProperties()", D3DERR_INVALIDCALL, currentUID);
  }
  return S_OK;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::SetCursorPosition(int X, int Y, DWORD Flags) {
  ZoneScoped;
  LogFunctionCall();

  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetCursorPosition, getId());
    c.send_many(X, Y, Flags);
  }
}

template<bool EnableSync>
BOOL Direct3DDevice9Ex_LSS<EnableSync>::ShowCursor(BOOL bShow) {
  ZoneScoped;
  LogFunctionCall();

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_ShowCursor, getId());
    currentUID = c.get_uid();
    c.send_data(bShow);
  }
  WAIT_FOR_SERVER_RESPONSE("ShowCursor()", false, currentUID);
  BOOL prevShow = (BOOL) DeviceBridge::get_data();
  DeviceBridge::pop_front();

  return prevShow;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** ppSwapChain) {
  ZoneScoped;
  LogFunctionCall();

  if (pPresentationParameters == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto presentationParameters = Direct3DSwapChain9_LSS::sanitizePresentationParameters(*pPresentationParameters, getCreateParams());

  // Insert our own IDirect3DTexture9 interface implementation
  Direct3DSwapChain9_LSS* pLssSwapChain = trackWrapper(new Direct3DSwapChain9_LSS(this, presentationParameters));
  (*ppSwapChain) = pLssSwapChain;

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateAdditionalSwapChain, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) pLssSwapChain->getId());
    c.send_data(sizeof(D3DPRESENT_PARAMETERS), &presentationParameters);
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateAdditionalSwapChain()", D3DERR_NOTAVAILABLE, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) {
  ZoneScoped;
  LogFunctionCall();

  if (pSwapChain == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pSwapChain = m_pSwapchain;
    m_pSwapchain->AddRef();
  }

  if (GlobalOptions::getSendReadOnlyCalls()) {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetSwapChain, getId());
    c.send_data(iSwapChain);
  }
  
  return S_OK;
}

template<bool EnableSync>
unsigned int Direct3DDevice9Ex_LSS<EnableSync>::GetNumberOfSwapChains() {
  ZoneScoped;
  LogFunctionCall();
  // DXVK does not support >1 implicit swapchains (those that are created during CreateDevice).
  static constexpr int kNumImplicitSwapChains = 1;
  if (GlobalOptions::getSendReadOnlyCalls()) {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetNumberOfSwapChains, getId());
    c.send_data(kNumImplicitSwapChains);
  }
  return kNumImplicitSwapChains;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
  ZoneScoped;
  LogFunctionCall();
  HRESULT res = S_OK;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    // Clear all device state and release implicit/internal objects
    releaseInternalObjects();
    // Reset all device state to default values and init implicit/internal objects
    ResetState();
    const auto presParam = Direct3DSwapChain9_LSS::sanitizePresentationParameters(*pPresentationParameters, getCreateParams());
    m_presParams = presParam;
    WndProc::unset();
    WndProc::set(getWinProcHwnd());
    // Tell Server to do the Reset
    size_t currentUID = 0;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_Reset, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(D3DPRESENT_PARAMETERS), &presParam);
    }

    // Perform an WAIT_FOR_OPTIONAL_SERVER_RESPONSE but don't return since we still have work to do.
    if (GlobalOptions::getSendAllServerResponses()) {
      const uint32_t timeoutMs = GlobalOptions::getAckTimeout();
      if (Result::Success != DeviceBridge::waitForCommand(Commands::Bridge_Response, timeoutMs, nullptr, true, currentUID)) {
        Logger::err("Direct3DDevice9Ex_LSS::Reset() failed with : no response from server.");
      }
      res = (HRESULT) DeviceBridge::get_data();
      DeviceBridge::pop_front();
      }

    // Reset swapchain and link server backbuffer/depth buffer after the server reset its swapchain, or we will link to the old backbuffer/depth resources
    initImplicitObjects(presParam);
    // Keeping a track of previous present parameters, to detect and handle mode changes
    m_previousPresentParams = *pPresentationParameters;
  }
  return res;
}

HRESULT syncOnPresent() {
#ifdef ENABLE_PRESENT_SEMAPHORE_TRACE
  Logger::trace("Client side Present call received, acquiring semaphore...");
#endif

  // If we're syncing with the server on Present() then wait for the semaphore to be released
  if (GlobalOptions::getPresentSemaphoreEnabled()) {
    const auto maxRetries = GlobalOptions::getCommandRetries();
    size_t numRetries = 0;
    while (gbBridgeRunning && RESULT_FAILURE(gpPresent->wait()) && numRetries++ < maxRetries) {
      Logger::warn("Still waiting on the Present semaphore to be released...");
    }
    if (numRetries >= maxRetries) {
      Logger::err("Max retries reached waiting on the Present semaphore!");
      return ERROR_SEM_TIMEOUT;
    } else if (!gbBridgeRunning) {
      Logger::err("Bridge was disabled while waiting on the Present semaphore, aborting current operation!");
      return ERROR_OPERATION_ABORTED;
#ifdef ENABLE_PRESENT_SEMAPHORE_TRACE
    } else {
      Logger::trace("Present semaphore acquired successfully.");
#endif
    }
  }
  return S_OK;
}

#include "sims3_device_lights.inl"    // the Sims 3 camera hook: the lamps and the lights of the sky
#include "sims3_device_present.inl"   // the Sims 3 camera hook: the statistics and the frame's end

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
  ZoneScoped;
  LogFunctionCall();

  // If the bridge was disabled in the meantime for some reason we want to bail
  // out here so we don't spend time waiting on the Present semaphore or trying
  // to send keyboard state to the server.
  if (!gbBridgeRunning) {
    return D3D_OK;
  }

  if (remixapi::g_bInterfaceInitialized && remixapi::g_presentCallback) {
    remixapi::g_presentCallback();
  }

  // The Sims 3 camera hook: the frame's end -- the diagnostics, the frame's bookkeeping, the lamps
  // and the lights of the sky (sims3_device_present.inl).
  if (sims3cam::enabled()) sims3OnPresent(g_sims3, this);

  return m_pSwapchain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, 0);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) {
  ZoneScoped;
  LogFunctionCall();
  return m_pSwapchain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) {
  ZoneScoped;
  LogFunctionCall();
  return m_pSwapchain->GetRasterStatus(pRasterStatus);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetDialogBoxMode(BOOL bEnableDialogs) {
  LogFunctionCall();

  UID currentUID = 0;
  // Send command to server and wait for response
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetDialogBoxMode, getId());
    currentUID = c.get_uid();
    c.send_data(bEnableDialogs);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetDialogBoxMode()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp) {
  ZoneScoped;
  LogFunctionCall();

  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_gammaRamp = *pRamp;
  }
  ClientMessage c(Commands::IDirect3DDevice9Ex_SetGammaRamp, getId());
  c.send_many(iSwapChain, Flags);
  c.send_data(sizeof(D3DGAMMARAMP), (void*) &m_gammaRamp);
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) {
  ZoneScoped;
  LogFunctionCall();

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pRamp = m_gammaRamp;
  }
  if (GlobalOptions::getSendReadOnlyCalls()) {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetGammaRamp, getId());
    c.send_data(iSwapChain);
  }
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // When Levels is 0, D3D9 will calculate the mip requirements
  if (Levels == 0) {
    Levels = CalculateNumMipLevels(Width, Height);
  }

  UID currentUID = 0;
  {
    const TEXTURE_DESC desc { Width, Height, 1, Levels, Usage, Format, Pool };
    auto* const pLssTexture = trackWrapper(new Direct3DTexture9_LSS(this, desc));
    (*ppTexture) = pLssTexture;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateTexture, getId());
      currentUID = c.get_uid();
      c.send_many(Width, Height, Levels, Usage, Format, Pool, (uint32_t) pLssTexture->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateTexture()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppVolumeTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // When Levels is 0, D3D9 will calculate the mip requirements
  if (Levels == 0) {
    Levels = CalculateNumMipLevels(Width, Height, Depth);
  }

  UID currentUID = 0;
  {
    const TEXTURE_DESC desc { Width, Height, Depth, Levels, Usage, Format, Pool };
    auto* const pLssVolumeTexture = trackWrapper(new Direct3DVolumeTexture9_LSS(this, desc));
    (*ppVolumeTexture) = pLssVolumeTexture;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateVolumeTexture, getId());
      currentUID = c.get_uid();
      c.send_many(Width, Height, Depth, Levels, Usage, Format, Pool, (uint32_t) pLssVolumeTexture->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateVolumeTexture()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppCubeTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // When Levels is 0, D3D9 will calculate the mip requirements
  if (Levels == 0) {
    Levels = CalculateNumMipLevels(EdgeLength);
  }

  UID currentUID = 0;
  {
    const TEXTURE_DESC desc { EdgeLength, EdgeLength, 6, Levels, Usage, Format, Pool };
    auto* const pLssCubeTexture = trackWrapper(new Direct3DCubeTexture9_LSS(this, desc));
    (*ppCubeTexture) = pLssCubeTexture;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateCubeTexture, getId());
      currentUID = c.get_uid();
      c.send_many(EdgeLength, Levels, Usage, Format, Pool, (uint32_t) pLssCubeTexture->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateCubeTexture()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();
  if (Length == 0) {
    return D3DERR_INVALIDCALL;
  }

  if (ppVertexBuffer == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const D3DVERTEXBUFFER_DESC desc { D3DFMT_VERTEXDATA, D3DRTYPE_VERTEXBUFFER, Usage, Pool, Length, FVF };
  UID currentUID = 0;
  {
    auto* const pLssVertexBuffer = trackWrapper(new Direct3DVertexBuffer9_LSS(this, desc));
    (*ppVertexBuffer) = pLssVertexBuffer;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateVertexBuffer, getId());
      currentUID = c.get_uid();
      c.send_many(Length, Usage, FVF, Pool, (uint32_t) pLssVertexBuffer->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateVertexBuffer()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (Length == 0) {
    return D3DERR_INVALIDCALL;
  }

  if (ppIndexBuffer == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const D3DINDEXBUFFER_DESC desc { Format, D3DRTYPE_INDEXBUFFER, Usage, Pool, Length };
  UID currentUID = 0;
  {
    auto* const pLssIndexBuffer = trackWrapper(new Direct3DIndexBuffer9_LSS(this, desc));
    (*ppIndexBuffer) = (IDirect3DIndexBuffer9*) pLssIndexBuffer;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateIndexBuffer, getId());
      currentUID = c.get_uid();
      c.send_many(Length, Usage, Format, Pool, (uint32_t) pLssIndexBuffer->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateIndexBuffer()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppSurface == nullptr || Width == 0 || Height == 0) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    D3DSURFACE_DESC desc;
    desc.Width = Width;
    desc.Height = Height;
    desc.Format = Format;
    desc.MultiSampleType = MultiSample;
    desc.MultiSampleQuality = MultisampleQuality;
    desc.Usage = D3DUSAGE_RENDERTARGET;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.Type = D3DRTYPE_SURFACE;

    // Insert our own IDirect3DSurface9 interface implementation
    Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
    (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

    {
      // Add a handle for the surface
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateRenderTarget, getId());
      currentUID = c.get_uid();
      c.send_many(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, pLssSurface->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateRenderTarget()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppSurface == nullptr || Width == 0 || Height == 0) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    D3DSURFACE_DESC desc;
    desc.Width = Width;
    desc.Height = Height;
    desc.Format = Format;
    desc.MultiSampleType = MultiSample;
    desc.MultiSampleQuality = MultisampleQuality;
    desc.Usage = D3DUSAGE_DEPTHSTENCIL;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.Type = D3DRTYPE_SURFACE;

    Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
    (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateDepthStencilSurface, getId());
      currentUID = c.get_uid();
      c.send_many(Width, Height, Format, MultiSample, MultisampleQuality, Discard, pLssSurface->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateDepthStencilSurface()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint) {
  ZoneScoped;
  LogFunctionCall();

  if (pSourceSurface == nullptr || pDestinationSurface == nullptr || pSourceSurface == pDestinationSurface) {
    return D3DERR_INVALIDCALL;
  }

  const auto pLssSrcSurface = bridge_cast<Direct3DSurface9_LSS*>(pSourceSurface);
  const auto pLssDestSurface = bridge_cast<Direct3DSurface9_LSS*>(pDestinationSurface);
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_UpdateSurface, getId());
    currentUID = c.get_uid();
    c.send_data(pLssSrcSurface->getId());
    c.send_data(sizeof(RECT), (void*) pSourceRect);
    c.send_data(pLssDestSurface->getId());
    c.send_data(sizeof(POINT), (void*) pDestPoint);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("UpdateSurface()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync> template<typename T>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::UpdateTextureImpl(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) {
  ZoneScoped;

  if (pSourceTexture == nullptr || pDestinationTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  auto pLssSourceTexture = bridge_cast<T*>(pSourceTexture);
  auto pLssDestinationTexture = bridge_cast<T*>(pDestinationTexture);
  assert(pLssSourceTexture && "UpdateTexture: unable to cast source texture!");
  assert(pLssDestinationTexture && "UpdateTexture: unable to cast destination texture!");
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_UpdateTexture, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) pLssSourceTexture->getId());
    c.send_data((uint32_t) pLssDestinationTexture->getId());
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("UpdateTextureImpl()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) {
  ZoneScoped;
  LogFunctionCall();

  assert(pSourceTexture->GetType() == pDestinationTexture->GetType() && "UpdateTexture: texture type mismatch!");

  switch (pSourceTexture->GetType()) {
  case D3DRTYPE_TEXTURE:
    return UpdateTextureImpl<Direct3DTexture9_LSS>(pSourceTexture, pDestinationTexture);
  case D3DRTYPE_CUBETEXTURE:
    return UpdateTextureImpl<Direct3DCubeTexture9_LSS>(pSourceTexture, pDestinationTexture);
  case D3DRTYPE_VOLUMETEXTURE:
    return UpdateTextureImpl<Direct3DVolumeTexture9_LSS>(pSourceTexture, pDestinationTexture);
  default:
    assert(0 && "UpdateTexture: unexpected texture type!");
  }

  return D3DERR_INVALIDCALL;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) {
  ZoneScoped;
  LogFunctionCall();

  const auto pLssSourceSurface = bridge_cast<Direct3DSurface9_LSS*>(pRenderTarget);
  const auto pLssDestinationSurface = bridge_cast<Direct3DSurface9_LSS*>(pDestSurface);

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetRenderTargetData, getId());
    currentUID = c.get_uid();
    c.send_data(pLssSourceSurface->getId());
    c.send_data(pLssDestinationSurface->getId());
  }

  // Wait for response from server
  return copyServerSurfaceRawData(pLssDestinationSurface, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) {
  ZoneScoped;
  LogFunctionCall();

  if (pDestSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto pLssDestinationSurface = bridge_cast<Direct3DSurface9_LSS*>(pDestSurface);

  UID currentUID = 0;
  {
    // Direct API call to server
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetFrontBufferData, getId());
    currentUID = c.get_uid();
    c.send_many(iSwapChain, pLssDestinationSurface->getId());
  }

  return copyServerSurfaceRawData(pLssDestinationSurface, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) {
  ZoneScoped;
  LogFunctionCall();

  if (pSourceSurface == nullptr || pDestSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  if (Filter != D3DTEXF_LINEAR && Filter != D3DTEXF_POINT
      && Filter != D3DTEXF_NONE) {
    return D3DERR_INVALIDCALL;
  }

  const auto pLssSrcSurface = bridge_cast<Direct3DSurface9_LSS*>(pSourceSurface);
  const auto pLssDstSurface = bridge_cast<Direct3DSurface9_LSS*>(pDestSurface);
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_StretchRect, getId());
    currentUID = c.get_uid();
    c.send_data(pLssSrcSurface->getId());
    c.send_data(sizeof(RECT), (void*) pSourceRect);
    c.send_data(pLssDstSurface->getId());
    c.send_data(sizeof(RECT), (void*) pDestRect);
    c.send_data(Filter);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("StretchRect()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color) {
  ZoneScoped;
  LogFunctionCall();

  if (pSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto pLssSurface = bridge_cast<Direct3DSurface9_LSS*>(pSurface);
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_ColorFill, getId());
    currentUID = c.get_uid();
    c.send_data(pLssSurface->getId());
    c.send_data(sizeof(RECT), (void*) pRect);
    c.send_data(sizeof(D3DCOLOR), (void*) &color);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("ColorFill()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    D3DSURFACE_DESC desc;
    desc.Width = Width;
    desc.Height = Height;
    desc.Format = Format;
    desc.MultiSampleType = D3DMULTISAMPLE_NONE;
    desc.MultiSampleQuality = 0;
    desc.Usage = D3DUSAGE_RENDERTARGET;
    desc.Pool = Pool;
    desc.Type = D3DRTYPE_SURFACE;

    // Insert our own IDirect3DSurface9 interface implementation
    Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
    (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

    {
      // Add a handle for the surface
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateOffscreenPlainSurface, getId());
      currentUID = c.get_uid();
      c.send_many(Width, Height, Format, Pool, pLssSurface->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateOffscreenPlainSurface()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
  ZoneScoped;
  LogFunctionCall();
  auto* const pLssRenderTarget = bridge_cast<Direct3DSurface9_LSS*>(pRenderTarget);
  // The Sims 3 camera hook: is render target 0 the backbuffer-sized target (the 3D pass)?
  if (sims3cam::enabled() && RenderTargetIndex == 0) {
    bool primary = true;
    g_sims3.rt0W = g_sims3.rt0H = 0; g_sims3.rt0Fmt = 0;
    if (pLssRenderTarget != nullptr) {
      D3DSURFACE_DESC d = {};
      if (SUCCEEDED(pLssRenderTarget->GetDesc(&d))) {
        g_sims3.rt0W = (uint16_t) d.Width; g_sims3.rt0H = (uint16_t) d.Height; g_sims3.rt0Fmt = (uint32_t) d.Format;
        if (m_pSwapchain != nullptr) {
          const D3DPRESENT_PARAMETERS& pp = m_pSwapchain->getPresentationParameters();
          primary = (d.Width == pp.BackBufferWidth && d.Height == pp.BackBufferHeight);
        }
      }
    }
    g_sims3.rtIsPrimary = primary;
  }
  UID currentUID = 0;
  {
    UID id = 0;
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (pLssRenderTarget) {
        m_state.renderTargets[RenderTargetIndex] = MakeD3DAutoPtr(pLssRenderTarget);
        id = pLssRenderTarget->getId();
      } else {
        m_state.renderTargets[RenderTargetIndex].reset(nullptr);
      }
    }

    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetRenderTarget, getId());
      currentUID = c.get_uid();
      c.send_many(RenderTargetIndex, id);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetRenderTarget()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) {
  ZoneScoped;
  LogFunctionCall();

  if (ppRenderTarget == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  Direct3DSurface9_LSS* pLssRenderTarget = nullptr;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    pLssRenderTarget = bridge_cast<Direct3DSurface9_LSS*>(*m_state.renderTargets[RenderTargetIndex]);
  }
  *ppRenderTarget = pLssRenderTarget;
  UID currentUID = 0;
  if (pLssRenderTarget) {
    pLssRenderTarget->AddRef();
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_GetRenderTarget, getId());
      currentUID = c.get_uid();
      c.send_many(RenderTargetIndex, pLssRenderTarget->getId());
    }
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("GetRenderTarget()", D3DERR_INVALIDCALL, currentUID);
  }
  
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
  ZoneScoped;
  LogFunctionCall();  
  UID currentUID = 0;
  {
    UID id = 0;
    {
      BRIDGE_DEVICE_LOCKGUARD();
      auto* const pLssDepthStencil = bridge_cast<Direct3DSurface9_LSS*>(pNewZStencil);
      if (pLssDepthStencil) {
        m_state.depthStencil = MakeD3DAutoPtr(pLssDepthStencil);
        id = pLssDepthStencil->getId();
      } else {
        m_state.depthStencil.reset(nullptr);
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetDepthStencilSurface, getId());
      currentUID = c.get_uid();
      c.send_data(id);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetDepthStencilSurface()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) {
  ZoneScoped;
  LogFunctionCall();

  if (ppZStencilSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    Direct3DSurface9_LSS* pLssDepthStencil = bridge_cast<Direct3DSurface9_LSS*>(*m_state.depthStencil);
    *ppZStencilSurface = pLssDepthStencil;
    if (pLssDepthStencil) {
      pLssDepthStencil->AddRef();
      {
        ClientMessage c(Commands::IDirect3DDevice9Ex_GetDepthStencilSurface, getId());
        currentUID = c.get_uid();
        c.send_data(pLssDepthStencil->getId());
      }
    } else {
      // No depth-stencil surface is currently bound. D3D9 semantics (and the DXVK
      // runtime one layer down) return D3DERR_NOTFOUND here with a NULL out-pointer.
      // Falling through to WAIT_FOR_OPTIONAL_SERVER_RESPONSE would return D3D_OK, which
      // tells callers that guard on FAILED(hr) that the NULL pointer is valid; The Sims 3
      // then dereferences it (IDirect3DSurface9::GetDesc on a null this) and crashes.
      return D3DERR_NOTFOUND;
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("GetDepthStencilSurface()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::BeginScene() {
  ZoneScoped;
  LogFunctionCall();

  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (gSceneState == WaitBeginScene) {
      gSceneState = SceneInProgress;
    }
  }
  
  if (remixapi::g_bInterfaceInitialized && remixapi::g_beginSceneCallback) {
    remixapi::g_beginSceneCallback();
  }

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_BeginScene, getId());
    currentUID = c.get_uid();
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("BeginScene()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::EndScene() {
  ZoneScoped;
  LogFunctionCall();

  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (gSceneState == SceneInProgress) {
      gSceneState = SceneEnded;
    }
  }

  if (remixapi::g_bInterfaceInitialized && remixapi::g_endSceneCallback) {
    remixapi::g_endSceneCallback();
  }

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_EndScene, getId());
    currentUID = c.get_uid();
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("EndScene()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
  ZoneScoped;
  LogFunctionCall();

  if (Count == 0 && pRects != NULL) {
    return D3DERR_INVALIDCALL;
  }
  if (Count != 0 && pRects == NULL) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_Clear, getId());
    currentUID = c.get_uid();
    c.send_many(Count, Flags);
    c.send_data(sizeof(float), &Z);
    c.send_data(Stencil);
    c.send_data(sizeof(D3DRECT) * Count, (void*) pRects);
    c.send_data(sizeof(D3DCOLOR), (void*) &Color);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("Clear()", D3DERR_INVALIDCALL, currentUID);
}

namespace {
  static inline size_t mapXformStateTypeToIdx(const D3DTRANSFORMSTATETYPE Type) {
    if (Type == D3DTS_VIEW) {
      return 0;
    }
    if (Type == D3DTS_PROJECTION) {
      return 1;
    }
    if (Type >= D3DTS_TEXTURE0 && Type <= D3DTS_TEXTURE7) {
      return 2 + (Type - D3DTS_TEXTURE0);
    }
    return 10 + (Type - D3DTS_WORLD);
  }
}

bool isValidD3drtansformstatetype(D3DTRANSFORMSTATETYPE Type) {
  if (Type == D3DTS_VIEW) {
    return true;
  }
  if (Type == D3DTS_PROJECTION) {
    return true;
  }
  if (Type >= D3DTS_TEXTURE0 && Type <= D3DTS_TEXTURE7) {
    return true;
  }
  if (Type >= D3DTS_WORLDMATRIX(0) && Type < D3DTS_WORLDMATRIX(256)) {
    return true;
  }
  return false;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) {
  ZoneScoped;
  LogFunctionCall();

  if (pMatrix == nullptr || !isValidD3drtansformstatetype(State)) {
    return D3DERR_INVALIDCALL;
  }

  const auto idx = mapXformStateTypeToIdx(State);
  // The Sims 3 camera hook: the game's own View / Projection, if it ever sets them (ours are marked)
  if (sims3cam::enabled() && !g_sims3.ourState) {
    if (State == D3DTS_VIEW) { g_sims3.gameXform[0] = *pMatrix; g_sims3.gameXformSet[0] = true; g_sims3.held.kind = sims3cam::Kind::None; }
    else if (State == D3DTS_PROJECTION) { g_sims3.gameXform[1] = *pMatrix; g_sims3.gameXformSet[1] = true; g_sims3.held.kind = sims3cam::Kind::None; }
  }
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.transforms[idx] &&
            memcmp(&m_stateRecording->m_captureState.transforms[idx], pMatrix, sizeof(D3DMATRIX)) == 0) {
          return S_OK;
        }
        m_stateRecording->m_captureState.transforms[idx] = *pMatrix;
        m_stateRecording->m_dirtyFlags.transforms[idx] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            memcmp(&m_state.transforms[idx], pMatrix, sizeof(D3DMATRIX)) == 0) {
          return S_OK;
        }
        m_state.transforms[idx] = *pMatrix;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetTransform, getId());
      currentUID = c.get_uid();
      c.send_data(State);
      c.send_data(sizeof(D3DMATRIX), (void*) pMatrix);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetTransform()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) {
  ZoneScoped;
  LogFunctionCall();

  if (pMatrix == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto idx = mapXformStateTypeToIdx(State);
  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pMatrix = m_state.transforms[idx];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) {
  ZoneScoped;
  LogFunctionCall();

  if (pMatrix == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  BRIDGE_DEVICE_LOCKGUARD();
  const auto idx = mapXformStateTypeToIdx(State);
  D3DMATRIX result = { 0 };
  D3DMATRIX current = (m_stateRecording && m_stateRecording->m_dirtyFlags.transforms[idx]) ? m_stateRecording->m_captureState.transforms[idx] : m_state.transforms[idx];

  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      float value = 0.0f;
      for (int k = 0; k < 4; k++) {
        value += current.m[i][k] * pMatrix->m[k][j];
      }
      result.m[i][j] = value;
    }
  }

  if (m_stateRecording) {
    m_stateRecording->m_captureState.transforms[idx] = result;
    m_stateRecording->m_dirtyFlags.transforms[idx] = true;
  } else {
    m_state.transforms[idx] = result;
  }
  
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetViewport(CONST D3DVIEWPORT9* pViewport) {
  ZoneScoped;
  LogFunctionCall();

  if (pViewport == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.viewport = *pViewport;
        m_stateRecording->m_dirtyFlags.viewport = true;
      } else {
        m_state.viewport = *pViewport;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetViewport, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(D3DVIEWPORT9), (void*) pViewport);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetViewport()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetViewport(D3DVIEWPORT9* pViewport) {
  ZoneScoped;
  LogFunctionCall();

  if (pViewport == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pViewport = m_state.viewport;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetMaterial(CONST D3DMATERIAL9* pMaterial) {
  ZoneScoped;
  LogFunctionCall();

  if (pMaterial == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.material = *pMaterial;
        m_stateRecording->m_dirtyFlags.material = true;
      } else {
        m_state.material = *pMaterial;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetMaterial, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(D3DMATERIAL9), (void*) pMaterial);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetMaterial()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetMaterial(D3DMATERIAL9* pMaterial) {
  ZoneScoped;
  LogFunctionCall();

  if (pMaterial == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pMaterial = m_state.material;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetLight(DWORD Index, CONST D3DLIGHT9* pLight) {
  ZoneScoped;
  LogFunctionCall();

  if (pLight == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.lights[Index] &&
            memcmp(&m_stateRecording->m_captureState.lights[Index], pLight, sizeof(D3DLIGHT9)) == 0) {
          return S_OK;
        }
        m_stateRecording->m_captureState.lights[Index] = *pLight;
        m_stateRecording->m_dirtyFlags.lights[Index] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            memcmp(&m_state.lights[Index], pLight, sizeof(D3DLIGHT9)) == 0) {
          return S_OK;
        }
        m_state.lights[Index] = *pLight;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetLight, getId());
      currentUID = c.get_uid();
      c.send_data(Index);
      c.send_data(sizeof(D3DLIGHT9), (void*) pLight);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetLight()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetLight(DWORD Index, D3DLIGHT9* pLight) {
  ZoneScoped;
  LogFunctionCall();

  if (pLight == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pLight = m_state.lights[Index];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::LightEnable(DWORD LightIndex, BOOL bEnable) {
  ZoneScoped;
  LogFunctionCall();

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.bLightEnables[LightIndex] &&
            (m_stateRecording->m_captureState.bLightEnables[LightIndex] == (bool)bEnable)) {
          return S_OK;
        }
        m_stateRecording->m_captureState.bLightEnables[LightIndex] = bEnable;
        m_stateRecording->m_dirtyFlags.bLightEnables[LightIndex] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_state.bLightEnables[LightIndex] == (bool)bEnable) {
          return S_OK;
        }
        m_state.bLightEnables[LightIndex] = bEnable;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_LightEnable, getId());
      currentUID = c.get_uid();
      c.send_many(LightIndex, (uint32_t) bEnable);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("LightEnable()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetLightEnable(DWORD Index, BOOL* pEnable) {
  ZoneScoped;
  LogFunctionCall();
  if (pEnable == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    // This is the true value for light-enables found through experimentation
    constexpr BOOL LightEnableTrue = 128;
    *pEnable = m_state.bLightEnables[Index] ? LightEnableTrue : 0;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetClipPlane(DWORD Index, CONST float* pPlane) {
  ZoneScoped;
  LogFunctionCall();

  if (pPlane == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_dirtyFlags.clipPlanes[Index] = true;
        for (int i = 0; i < 4; i++) {
          m_stateRecording->m_captureState.clipPlanes[Index][i] = pPlane[i];
        }
      } else {
        for (int i = 0; i < 4; i++) {
          m_state.clipPlanes[Index][i] = pPlane[i];
        }
      }
    }
    {
      // pPlane is a four-element array with the clipping plane coefficients
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetClipPlane, getId());
      currentUID = c.get_uid();
      c.send_data(Index);
      c.send_data(sizeof(float) * 4, (void*) pPlane);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetClipPlane()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetClipPlane(DWORD Index, float* pPlane) {
  ZoneScoped;
  LogFunctionCall();

  if (pPlane == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    for (int i = 0; i < 4; i++) {
      pPlane[i] = m_state.clipPlanes[Index][i];
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
  ZoneScoped;
  LogFunctionCall();
  // The Sims 3 camera hook: the game's own render states (not the hook's) are trusted from here on
  if (sims3cam::enabled() && !g_sims3.ourState) {
    if ((DWORD) State < 256) g_sims3.rsSet[State] = true;
    if (State == D3DRS_TEXTUREFACTOR) { g_sims3.gameFactor = Value; g_sims3.factorOurs = false; g_sims3.sentFactor = Value; }
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.renderStates[State] && m_stateRecording->m_captureState.renderStates[State] == Value) {
          return S_OK;
        }
        m_stateRecording->m_captureState.renderStates[State] = Value;
        m_stateRecording->m_dirtyFlags.renderStates[State] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_state.renderStates[State] == Value) {
          return S_OK;
        }
        m_state.renderStates[State] = Value;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetRenderState, getId());
      currentUID = c.get_uid();
      c.send_many(State, Value);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetRenderState()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) {
  ZoneScoped;
  LogFunctionCall();

  if (pValue == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  
  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pValue = m_state.renderStates[State];
  }
  return S_OK;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::StateBlockSetPixelCaptureFlags(BaseDirect3DDevice9Ex_LSS::StateCaptureDirtyFlags& flags) {
  flags.renderStates[D3DRS_ZENABLE] = true;
  flags.renderStates[D3DRS_FILLMODE] = true;
  flags.renderStates[D3DRS_SHADEMODE] = true;
  flags.renderStates[D3DRS_ZWRITEENABLE] = true;
  flags.renderStates[D3DRS_ALPHATESTENABLE] = true;
  flags.renderStates[D3DRS_LASTPIXEL] = true;
  flags.renderStates[D3DRS_SRCBLEND] = true;
  flags.renderStates[D3DRS_DESTBLEND] = true;
  flags.renderStates[D3DRS_ZFUNC] = true;
  flags.renderStates[D3DRS_ALPHAREF] = true;
  flags.renderStates[D3DRS_ALPHAFUNC] = true;
  flags.renderStates[D3DRS_DITHERENABLE] = true;
  flags.renderStates[D3DRS_FOGSTART] = true;
  flags.renderStates[D3DRS_FOGEND] = true;
  flags.renderStates[D3DRS_FOGDENSITY] = true;
  flags.renderStates[D3DRS_ALPHABLENDENABLE] = true;
  flags.renderStates[D3DRS_DEPTHBIAS] = true;
  flags.renderStates[D3DRS_STENCILENABLE] = true;
  flags.renderStates[D3DRS_STENCILFAIL] = true;
  flags.renderStates[D3DRS_STENCILZFAIL] = true;
  flags.renderStates[D3DRS_STENCILPASS] = true;
  flags.renderStates[D3DRS_STENCILFUNC] = true;
  flags.renderStates[D3DRS_STENCILREF] = true;
  flags.renderStates[D3DRS_STENCILMASK] = true;
  flags.renderStates[D3DRS_STENCILWRITEMASK] = true;
  flags.renderStates[D3DRS_TEXTUREFACTOR] = true;
  flags.renderStates[D3DRS_WRAP0] = true;
  flags.renderStates[D3DRS_WRAP1] = true;
  flags.renderStates[D3DRS_WRAP2] = true;
  flags.renderStates[D3DRS_WRAP3] = true;
  flags.renderStates[D3DRS_WRAP4] = true;
  flags.renderStates[D3DRS_WRAP5] = true;
  flags.renderStates[D3DRS_WRAP6] = true;
  flags.renderStates[D3DRS_WRAP7] = true;
  flags.renderStates[D3DRS_WRAP8] = true;
  flags.renderStates[D3DRS_WRAP9] = true;
  flags.renderStates[D3DRS_WRAP10] = true;
  flags.renderStates[D3DRS_WRAP11] = true;
  flags.renderStates[D3DRS_WRAP12] = true;
  flags.renderStates[D3DRS_WRAP13] = true;
  flags.renderStates[D3DRS_WRAP14] = true;
  flags.renderStates[D3DRS_WRAP15] = true;
  flags.renderStates[D3DRS_COLORWRITEENABLE] = true;
  flags.renderStates[D3DRS_BLENDOP] = true;
  flags.renderStates[D3DRS_SCISSORTESTENABLE] = true;
  flags.renderStates[D3DRS_SLOPESCALEDEPTHBIAS] = true;
  flags.renderStates[D3DRS_ANTIALIASEDLINEENABLE] = true;
  flags.renderStates[D3DRS_TWOSIDEDSTENCILMODE] = true;
  flags.renderStates[D3DRS_CCW_STENCILFAIL] = true;
  flags.renderStates[D3DRS_CCW_STENCILZFAIL] = true;
  flags.renderStates[D3DRS_CCW_STENCILPASS] = true;
  flags.renderStates[D3DRS_CCW_STENCILFUNC] = true;
  flags.renderStates[D3DRS_COLORWRITEENABLE1] = true;
  flags.renderStates[D3DRS_COLORWRITEENABLE2] = true;
  flags.renderStates[D3DRS_COLORWRITEENABLE3] = true;
  flags.renderStates[D3DRS_BLENDFACTOR] = true;
  flags.renderStates[D3DRS_SRGBWRITEENABLE] = true;
  flags.renderStates[D3DRS_SEPARATEALPHABLENDENABLE] = true;
  flags.renderStates[D3DRS_SRCBLENDALPHA] = true;
  flags.renderStates[D3DRS_DESTBLENDALPHA] = true;
  flags.renderStates[D3DRS_BLENDOPALPHA] = true;

  for (uint32_t i = 0; i < caps::MaxTexturesPS + 1; i++) {
    flags.samplerStates[i][D3DSAMP_ADDRESSU] = true;
    flags.samplerStates[i][D3DSAMP_ADDRESSV] = true;
    flags.samplerStates[i][D3DSAMP_ADDRESSW] = true;
    flags.samplerStates[i][D3DSAMP_BORDERCOLOR] = true;
    flags.samplerStates[i][D3DSAMP_MAGFILTER] = true;
    flags.samplerStates[i][D3DSAMP_MINFILTER] = true;
    flags.samplerStates[i][D3DSAMP_MIPFILTER] = true;
    flags.samplerStates[i][D3DSAMP_MIPMAPLODBIAS] = true;
    flags.samplerStates[i][D3DSAMP_MAXMIPLEVEL] = true;
    flags.samplerStates[i][D3DSAMP_MAXANISOTROPY] = true;
    flags.samplerStates[i][D3DSAMP_SRGBTEXTURE] = true;
    flags.samplerStates[i][D3DSAMP_ELEMENTINDEX] = true;
  }
  for (auto& fConst : flags.pixelConstants.fConsts) {
    fConst = true;
  }
  for (auto& iConst : flags.pixelConstants.iConsts) {
    iConst = true;
  }
  for (auto& bConst : flags.pixelConstants.bConsts) {
    bConst = true;
  }
  for (auto& stage : flags.textureStageStates) {
    std::fill(std::begin(stage), std::end(stage), true);
  }
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::StateBlockSetVertexCaptureFlags(BaseDirect3DDevice9Ex_LSS::StateCaptureDirtyFlags& flags) {
  flags.renderStates[D3DRS_CULLMODE] = true;
  flags.renderStates[D3DRS_FOGENABLE] = true;
  flags.renderStates[D3DRS_FOGCOLOR] = true;
  flags.renderStates[D3DRS_FOGTABLEMODE] = true;
  flags.renderStates[D3DRS_FOGSTART] = true;
  flags.renderStates[D3DRS_FOGEND] = true;
  flags.renderStates[D3DRS_FOGDENSITY] = true;
  flags.renderStates[D3DRS_RANGEFOGENABLE] = true;
  flags.renderStates[D3DRS_AMBIENT] = true;
  flags.renderStates[D3DRS_COLORVERTEX] = true;
  flags.renderStates[D3DRS_FOGVERTEXMODE] = true;
  flags.renderStates[D3DRS_CLIPPING] = true;
  flags.renderStates[D3DRS_LIGHTING] = true;
  flags.renderStates[D3DRS_LOCALVIEWER] = true;
  flags.renderStates[D3DRS_EMISSIVEMATERIALSOURCE] = true;
  flags.renderStates[D3DRS_AMBIENTMATERIALSOURCE] = true;
  flags.renderStates[D3DRS_DIFFUSEMATERIALSOURCE] = true;
  flags.renderStates[D3DRS_SPECULARMATERIALSOURCE] = true;
  flags.renderStates[D3DRS_VERTEXBLEND] = true;
  flags.renderStates[D3DRS_CLIPPLANEENABLE] = true;
  flags.renderStates[D3DRS_POINTSIZE] = true;
  flags.renderStates[D3DRS_POINTSIZE_MIN] = true;
  flags.renderStates[D3DRS_POINTSPRITEENABLE] = true;
  flags.renderStates[D3DRS_POINTSCALEENABLE] = true;
  flags.renderStates[D3DRS_POINTSCALE_A] = true;
  flags.renderStates[D3DRS_POINTSCALE_B] = true;
  flags.renderStates[D3DRS_POINTSCALE_C] = true;
  flags.renderStates[D3DRS_MULTISAMPLEANTIALIAS] = true;
  flags.renderStates[D3DRS_MULTISAMPLEMASK] = true;
  flags.renderStates[D3DRS_PATCHEDGESTYLE] = true;
  flags.renderStates[D3DRS_POINTSIZE_MAX] = true;
  flags.renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE] = true;
  flags.renderStates[D3DRS_TWEENFACTOR] = true;
  flags.renderStates[D3DRS_POSITIONDEGREE] = true;
  flags.renderStates[D3DRS_NORMALDEGREE] = true;
  flags.renderStates[D3DRS_MINTESSELLATIONLEVEL] = true;
  flags.renderStates[D3DRS_MAXTESSELLATIONLEVEL] = true;
  flags.renderStates[D3DRS_ADAPTIVETESS_X] = true;
  flags.renderStates[D3DRS_ADAPTIVETESS_Y] = true;
  flags.renderStates[D3DRS_ADAPTIVETESS_Z] = true;
  flags.renderStates[D3DRS_ADAPTIVETESS_W] = true;
  flags.renderStates[D3DRS_ENABLEADAPTIVETESSELLATION] = true;
  flags.renderStates[D3DRS_NORMALIZENORMALS] = true;
  flags.renderStates[D3DRS_SPECULARENABLE] = true;
  flags.renderStates[D3DRS_SHADEMODE] = true;

  flags.vertexDecl = true;
  std::fill(std::begin(flags.streamFreqs), std::end(flags.streamFreqs), true);
  // Lights in the map are always transferred if they exist 
  // LightEnables in the map are always transferred if they exist 
  for (uint32_t i = caps::MaxTexturesPS + 1; i < BaseDirect3DDevice9Ex_LSS::kMaxStageSamplerStateTypes; i++) {
    flags.samplerStates[i][D3DSAMP_DMAPOFFSET] = true;
  }

  for (auto& fConst : flags.vertexConstants.fConsts) {
    fConst = true;
  }
  for (auto& iConst : flags.vertexConstants.iConsts) {
    iConst = true;
  }
  for (auto& bConst : flags.vertexConstants.bConsts) {
    bConst = true;
  }

  for (uint32_t i = 0; i < flags.streamFreqs.size(); i++) {
    flags.streamFreqs[i] = true;
  }

}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::StateBlockSetCaptureFlags(D3DSTATEBLOCKTYPE Type, BaseDirect3DDevice9Ex_LSS::StateCaptureDirtyFlags& flags) {
  if (Type == D3DSBT_PIXELSTATE || Type == D3DSBT_ALL) {
    StateBlockSetPixelCaptureFlags(flags);
  }
  if (Type == D3DSBT_VERTEXSTATE || Type == D3DSBT_ALL) {
    StateBlockSetVertexCaptureFlags(flags);
  }
  if (Type == D3DSBT_ALL) {
    std::fill(std::begin(flags.textures), std::end(flags.textures), true);
    std::fill(std::begin(flags.streams), std::end(flags.streams), true);
    std::fill(std::begin(flags.streamOffsetsAndStrides), std::end(flags.streamOffsetsAndStrides), true);

    flags.indices = true;
    flags.viewport = true;
    flags.scissorRect = true;

    std::fill(std::begin(flags.clipPlanes), std::end(flags.clipPlanes), true);

    std::fill(std::begin(flags.transforms), std::end(flags.transforms), true);

    flags.material = true;
  }
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) {
  ZoneScoped;
  LogFunctionCall();

  if (ppSB == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    
    Direct3DStateBlock9_LSS* pLssSB = nullptr;
    {
      BRIDGE_DEVICE_LOCKGUARD();
      // Insert our own IDirect3DStateBlock9 interface implementation
      pLssSB = trackWrapper(new Direct3DStateBlock9_LSS(this));
      (*ppSB) = pLssSB;
      StateBlockSetCaptureFlags(Type, pLssSB->m_dirtyFlags);
      pLssSB->LocalCapture();
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateStateBlock, getId());
      currentUID = c.get_uid();
      c.send_many(Type, (uint32_t) pLssSB->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateStateBlock()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::BeginStateBlock() {
  ZoneScoped;
  LogFunctionCall();

  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (m_stateRecording) {
      return D3DERR_INVALIDCALL;
    }
    m_stateRecording = trackWrapper(new Direct3DStateBlock9_LSS(this));
  }
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_BeginStateBlock, getId());
    currentUID = c.get_uid();
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("BeginStateBlock()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::EndStateBlock(IDirect3DStateBlock9** ppSB) {
  ZoneScoped;
  LogFunctionCall();

  if (ppSB == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  BRIDGE_DEVICE_LOCKGUARD();

  if (!m_stateRecording) {
    return D3DERR_INVALIDCALL;
  }
  (*ppSB) = m_stateRecording;

  UID currentUID = 0;
  {
    // Add a handle for the sb
    ClientMessage c(Commands::IDirect3DDevice9Ex_EndStateBlock, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) m_stateRecording->getId());
    m_stateRecording = nullptr;
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("EndStateBlock()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus) {
  ZoneScoped;
  LogFunctionCall();

  if (pClipStatus == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_clipStatus = *pClipStatus;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetClipStatus(D3DCLIPSTATUS9* pClipStatus) {
  ZoneScoped;
  LogFunctionCall();

  if (pClipStatus == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pClipStatus = m_clipStatus;
  }
  return S_OK;
}

namespace {
  static inline bool isInvalidSamplerStage(const DWORD samplerStage) {
    if (samplerStage > 15 && samplerStage < D3DDMAPSAMPLER) {
      return true;
    }
    if (samplerStage > D3DVERTEXTEXTURESAMPLER3) {
      return true;
    }
    return false;
  }
  static inline DWORD mapSamplerStageToIdx(const DWORD samplerStage) {
    if (samplerStage >= D3DDMAPSAMPLER) {
      return caps::MaxTexturesPS + (samplerStage - D3DDMAPSAMPLER);
    }
    return samplerStage;
  }
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
  ZoneScoped;
  LogFunctionCall();
  if (isInvalidSamplerStage(Stage) || ppTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto idx = mapSamplerStageToIdx(Stage);
  {
    BRIDGE_DEVICE_LOCKGUARD();

    if (*m_state.textures[idx] != nullptr) {
      switch (m_state.textureTypes[idx]) {
      case D3DRTYPE_TEXTURE:
      {
        (*ppTexture) = bridge_cast<Direct3DTexture9_LSS*>(*m_state.textures[idx]);
        break;
      }
      case D3DRTYPE_CUBETEXTURE:
      {
        (*ppTexture) = bridge_cast<Direct3DCubeTexture9_LSS*>(*m_state.textures[idx]);
        break;
      }
      case D3DRTYPE_VOLUMETEXTURE:
      {
        (*ppTexture) = bridge_cast<Direct3DVolumeTexture9_LSS*>(*m_state.textures[idx]);
        break;
      }
      default:
        assert(0);
        return E_FAIL;
      }
      if ((*ppTexture)) {
        (*ppTexture)->AddRef();
      }
    } else {
      *ppTexture = nullptr;
      return S_OK;
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
  ZoneScoped;
  LogFunctionCall();

  if (isInvalidSamplerStage(Stage)) {
    return D3DERR_INVALIDCALL;
  }

  // The Sims 3 camera hook: track what the game binds per stage (not our own remap calls).
  if (sims3cam::enabled() && !g_sims3.inRemap && Stage < 16 && !m_stateRecording) sims3NoteTexture(g_sims3, Stage, pTexture);

  IDirect3DBaseTexture9* pD3DObject = nullptr;
  D3DAutoPtr objectRef;

  const auto idx = mapSamplerStageToIdx(Stage);

  D3DRESOURCETYPE type = D3DRTYPE_FORCE_DWORD;

  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (pTexture != nullptr) {
      switch (pTexture->GetType()) {
      case D3DRTYPE_TEXTURE:
      {
        auto* const pLssTexture = bridge_cast<Direct3DTexture9_LSS*>(pTexture);
        pD3DObject = (pLssTexture->D3D<IDirect3DBaseTexture9>());
        objectRef = MakeD3DAutoPtr(pLssTexture);
        break;
      }
      case D3DRTYPE_CUBETEXTURE:
      {
        auto* const pLssCubeTexture = bridge_cast<Direct3DCubeTexture9_LSS*>(pTexture);
        pD3DObject = (pLssCubeTexture->D3D<IDirect3DBaseTexture9>());
        objectRef = MakeD3DAutoPtr(pLssCubeTexture);
        break;
      }
      case D3DRTYPE_VOLUMETEXTURE:
      {
        auto* const pLssVolumeTexture = bridge_cast<Direct3DVolumeTexture9_LSS*>(pTexture);
        pD3DObject = (pLssVolumeTexture->D3D<IDirect3DBaseTexture9>());
        objectRef = MakeD3DAutoPtr(pLssVolumeTexture);
        break;
      }
      default:
        assert(0);
        return E_FAIL;
      }
      type = pTexture->GetType();
    }
    if (m_stateRecording) {
      m_stateRecording->m_captureState.textures[idx] = std::move(objectRef);
      m_stateRecording->m_captureState.textureTypes[idx] = type;
      m_stateRecording->m_dirtyFlags.textures[idx] = true;
    } else {
      m_state.textures[idx] = std::move(objectRef);
      m_state.textureTypes[idx] = type;
    }
  }
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetTexture, getId());
    currentUID = c.get_uid();
    c.send_many(Stage, (uint32_t) pD3DObject);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetTexture()", D3DERR_INVALIDCALL, currentUID);
}

namespace {
  enum TextureStageStateType {
    ColorOp = 0,
    ColorArg1 = 1,
    ColorArg2 = 2,
    AlphaOp = 3,
    AlphaArg1 = 4,
    AlphaArg2 = 5,
    BumpEnvMat00 = 6,
    BumpEnvMat01 = 7,
    BumpEnvMat10 = 8,
    BumpEnvMat11 = 9,
    TexCoordIdx = 10,
    BumpEnvLScale = 11,
    BumpEnvLOffset = 12,
    TexXformFlags = 13,
    ColorArg0 = 14,
    AlphaArg0 = 15,
    ResultArg = 16,
    Constant = 17,
    kCount
  };
  static_assert(BaseDirect3DDevice9Ex_LSS::kMaxTexStageStateTypes == (size_t) TextureStageStateType::kCount);
  static size_t TexStageStateTypeToIdx(const D3DTEXTURESTAGESTATETYPE type) {
    switch (type) {
    case D3DTSS_COLOROP: return (size_t) TextureStageStateType::ColorOp;
    case D3DTSS_COLORARG1: return (size_t) TextureStageStateType::ColorArg1;
    case D3DTSS_COLORARG2: return (size_t) TextureStageStateType::ColorArg2;
    case D3DTSS_ALPHAOP: return (size_t) TextureStageStateType::AlphaOp;
    case D3DTSS_ALPHAARG1: return (size_t) TextureStageStateType::AlphaArg1;
    case D3DTSS_ALPHAARG2: return (size_t) TextureStageStateType::AlphaArg2;
    case D3DTSS_BUMPENVMAT00: return (size_t) TextureStageStateType::BumpEnvMat00;
    case D3DTSS_BUMPENVMAT01: return (size_t) TextureStageStateType::BumpEnvMat01;
    case D3DTSS_BUMPENVMAT10: return (size_t) TextureStageStateType::BumpEnvMat10;
    case D3DTSS_BUMPENVMAT11: return (size_t) TextureStageStateType::BumpEnvMat11;
    case D3DTSS_TEXCOORDINDEX: return (size_t) TextureStageStateType::TexCoordIdx;
    case D3DTSS_BUMPENVLSCALE: return (size_t) TextureStageStateType::BumpEnvLScale;
    case D3DTSS_BUMPENVLOFFSET: return (size_t) TextureStageStateType::BumpEnvLOffset;
    case D3DTSS_TEXTURETRANSFORMFLAGS: return (size_t) TextureStageStateType::TexXformFlags;
    case D3DTSS_COLORARG0: return (size_t) TextureStageStateType::ColorArg0;
    case D3DTSS_ALPHAARG0: return (size_t) TextureStageStateType::AlphaArg0;
    case D3DTSS_RESULTARG: return (size_t) TextureStageStateType::ResultArg;
    case D3DTSS_CONSTANT: return (size_t) TextureStageStateType::Constant;
    }
    return (size_t) -1;
  }
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) {
  ZoneScoped;
  LogFunctionCall();
  if (isInvalidSamplerStage(Stage) || pValue == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  const auto typeIdx = TexStageStateTypeToIdx(Type);
  if (typeIdx >= kMaxTexStageStateTypes) {
    return D3DERR_INVALIDCALL;
  }
  const auto stageIdx = mapSamplerStageToIdx(Stage);
  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pValue = m_state.textureStageStates[stageIdx][typeIdx];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  ZoneScoped;
  LogFunctionCall();
  if (Stage >= caps::MaxSimultaneousTextures) {
    return D3DERR_INVALIDCALL;
  }
  const auto typeIdx = TexStageStateTypeToIdx(Type);
  if (typeIdx >= kMaxTexStageStateTypes) {
    return D3DERR_INVALIDCALL;
  }
  const auto stageIdx = mapSamplerStageToIdx(Stage);
  // The Sims 3 camera hook: the game's own stage-0 values (ours are marked), put back before uncaptured draws
  if (sims3cam::enabled() && !g_sims3.ourState && Stage == 0) {
    const int k = Type == D3DTSS_COLOROP ? 0 : Type == D3DTSS_COLORARG1 ? 1 : Type == D3DTSS_COLORARG2 ? 2 : Type == D3DTSS_TEXCOORDINDEX ? 3 : -1;
    if (k >= 0) {
      g_sims3.gameTss0[k] = Value;
      if (k == 3) g_sims3.uvIndexHidden = false; else g_sims3.tssOurs &= (uint8_t) ~(1u << k);   // the runtime now holds the game's value; ours is re-applied at the next captured draw
    }
  }
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.textureStageStates[stageIdx][typeIdx] && m_stateRecording->m_captureState.textureStageStates[stageIdx][typeIdx] == Value) {
          return S_OK;
        }
        m_stateRecording->m_captureState.textureStageStates[stageIdx][typeIdx] = Value;
        m_stateRecording->m_dirtyFlags.textureStageStates[stageIdx][typeIdx] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_state.textureStageStates[stageIdx][typeIdx] == Value) {
          return S_OK;
        }
        m_state.textureStageStates[stageIdx][typeIdx] = Value;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetTextureStageState, getId());
      currentUID = c.get_uid();
      c.send_many(Stage, Type, Value);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetTextureStageState()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) {
  ZoneScoped;
  LogFunctionCall();
  if (isInvalidSamplerStage(Sampler) || pValue == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  const auto typeIdx = Type - 1;
  if (typeIdx >= kMaxStageSamplerStateTypes) {
    return D3DERR_INVALIDCALL;
  }
  const auto samplerIdx = mapSamplerStageToIdx(Sampler);
  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pValue = m_state.samplerStates[samplerIdx][typeIdx];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
  ZoneScoped;
  LogFunctionCall();
  if (isInvalidSamplerStage(Sampler)) {
    return D3DERR_INVALIDCALL;
  }
  const auto typeIdx = Type - 1;
  if (typeIdx >= kMaxStageSamplerStateTypes) {
    return D3DERR_INVALIDCALL;
  }
  const auto samplerIdx = mapSamplerStageToIdx(Sampler);
  // The Sims 3 camera hook (milestone 18g): the game setting a sampler state the terrain block
  // holds cancels that state's restore -- the game's value is the current one
  if (sims3cam::enabled() && !g_sims3.ourSampler && !m_stateRecording && g_sims3.tblockActive) {
    auto& h = g_sims3;
    if ((int) Sampler == h.tblockStage) for (int i = 0; i < Sims3Hook::kSamplerCopies; ++i) if (kSims3SamplerCopy[i] == Type && (h.tblockSet & (1u << i))) { h.tblockSet &= (uint8_t) ~(1u << i); ++h.tblockCancelled; }
    if (Type == D3DSAMP_SRGBTEXTURE && Sampler < 16 && (h.tblockSrgb & (1u << Sampler))) { h.tblockSrgb &= (uint16_t) ~(1u << Sampler); ++h.tblockCancelled; }
  }
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.samplerStates[samplerIdx][typeIdx] &&
            m_stateRecording->m_captureState.samplerStates[samplerIdx][typeIdx] == Value) {
          return S_OK;
        }
        m_stateRecording->m_captureState.samplerStates[samplerIdx][typeIdx] = Value;
        m_stateRecording->m_dirtyFlags.samplerStates[samplerIdx][typeIdx] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() && 
            m_state.samplerStates[samplerIdx][typeIdx] == Value) {
          return S_OK;
        }
        m_state.samplerStates[samplerIdx][typeIdx] = Value;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetSamplerState, getId());
      currentUID = c.get_uid();
      c.send_many(Sampler, Type, Value);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetSamplerState()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ValidateDevice(DWORD* pNumPasses) {
  ZoneScoped;
  LogFunctionCall();

  if (pNumPasses == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // Since we're running graphics on a strictly better graphics API and HW,
  // always return 1 rendering pass which is the best case for d3d8/d3d9.
  *pNumPasses = 1;

  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries) {
  ZoneScoped;
  LogFunctionCall();

  if (pEntries == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_paletteEntries[PaletteNumber] = *pEntries;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) {
  ZoneScoped;
  LogFunctionCall();

  if (pEntries == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pEntries = m_paletteEntries[PaletteNumber];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetCurrentTexturePalette(UINT PaletteNumber) {
  ZoneScoped;
  LogFunctionCall();
  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_curTexPalette = PaletteNumber;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetCurrentTexturePalette(UINT* pPaletteNumber) {
  ZoneScoped;
  LogFunctionCall();

  if (pPaletteNumber == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pPaletteNumber = m_curTexPalette;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetScissorRect(CONST RECT* pRect) {
  ZoneScoped;
  LogFunctionCall();

  if (pRect == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.scissorRect = *pRect;
        m_stateRecording->m_dirtyFlags.scissorRect = true;
      } else {
        m_state.scissorRect = *pRect;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetScissorRect, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(RECT), (void*) pRect);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetScissorRect()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetScissorRect(RECT* pRect) {
  ZoneScoped;
  LogFunctionCall();

  if (pRect == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pRect = m_state.scissorRect;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetSoftwareVertexProcessing(BOOL bSoftware) {
  ZoneScoped;
  LogFunctionCall();
  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (m_bSoftwareVtxProcessing == bSoftware)
      return D3D_OK;
    m_bSoftwareVtxProcessing = bSoftware;
  }
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetSoftwareVertexProcessing, getId());
    currentUID = c.get_uid();
    c.send_data(bSoftware);
  }
  WAIT_FOR_SERVER_RESPONSE("SetSoftwareVertexProcessing()", D3DERR_INVALIDCALL, currentUID);

  HRESULT hresult = DeviceBridge::get_data();
  DeviceBridge::pop_front();

  return hresult;
}

template<bool EnableSync>
int Direct3DDevice9Ex_LSS<EnableSync>::GetSoftwareVertexProcessing() {
  ZoneScoped;
  LogFunctionCall();
  BOOL result;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    result = m_bSoftwareVtxProcessing;
  }
  return result;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetNPatchMode(float nSegments) {
  LogFunctionCall();
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      m_NPatchMode = nSegments;
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetNPatchMode, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(float), &nSegments);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetNPatchMode()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
float Direct3DDevice9Ex_LSS<EnableSync>::GetNPatchMode() {
  ZoneScoped;
  LogFunctionCall();
  float result;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    result = m_NPatchMode;
  }
  return result;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
  ZoneScoped;
  LogFunctionCall();
  if (sims3cam::enabled()) g_sims3.drawIndexed = false;
  SIMS3_BEGIN_DRAW();
  UID currentUID = 0;
  if (sims3cam::enabled() && g_sims3.splitDraw && PrimitiveType == D3DPT_TRIANGLELIST && PrimitiveCount >= 2) {   // a lot's re-submission as two half draws (milestone 17r)
    const UINT half_ = PrimitiveCount / 2;
    { ClientMessage c(Commands::IDirect3DDevice9Ex_DrawPrimitive, getId()); currentUID = c.get_uid(); c.send_many(PrimitiveType, StartVertex, half_); }
    { ClientMessage c(Commands::IDirect3DDevice9Ex_DrawPrimitive, getId()); currentUID = c.get_uid(); c.send_many(PrimitiveType, StartVertex + half_ * 3, PrimitiveCount - half_); }
    ++g_sims3.splitDraws;
  } else {
    ClientMessage c(Commands::IDirect3DDevice9Ex_DrawPrimitive, getId());
    currentUID = c.get_uid();
    c.send_many(PrimitiveType, StartVertex, PrimitiveCount);
  }
  if (sims3cam::enabled() && g_sims3.compositeSecond) { SIMS3_END_DRAW(); sims3CompositeSecondPass(g_sims3, this, false, PrimitiveType, 0, 0, 0, StartVertex, PrimitiveCount); } else
  SIMS3_END_DRAW();
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("DrawPrimitive()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) {
  ZoneScoped;
  LogFunctionCall();
  if (sims3cam::enabled()) { g_sims3.drawIndexed = true; g_sims3.drawType = Type; g_sims3.drawBase = BaseVertexIndex; g_sims3.drawStart = startIndex; g_sims3.drawPrims = primCount; }   // for the squares (milestone 60)
  SIMS3_BEGIN_DRAW();
  // The Sims 3 camera hook: the directional light a lit terrain shader is handed for this draw, c0
  // its colour and c1 the direction toward it: the sun, or the moon (milestone 41). Present
  // forwards what the frame's last such draw was given.
  if (sims3cam::enabled() && primCount > 0 && sims3cam::isLitTerrainPs(g_sims3.psHash)) {
    memcpy(g_sims3.terrainSunCol, &m_state.pixelConstants.fConsts[0], sizeof g_sims3.terrainSunCol);
    memcpy(g_sims3.terrainSunDir, &m_state.pixelConstants.fConsts[1], sizeof g_sims3.terrainSunDir);
    g_sims3.terrainNightSwitch = m_state.pixelConstants.fConsts[7].data[0];   // c7.x: the game's night switch (milestone 54)
    memcpy(g_sims3.terrainFog, &m_state.pixelConstants.fConsts[2], 4 * sizeof(float));       // c2: the game's fog colour (milestone 56)
    memcpy(g_sims3.terrainFog + 4, &m_state.pixelConstants.fConsts[4], 4 * sizeof(float));   // c4: the game's fog range and curve
    ++g_sims3.terrainSunDraws;
  }
  // The Sims 3 camera hook: a captured draw whose render states make it invisible in-game
  // (colour writes off, a depth test that never passes, an alpha test that never passes, a
  // blend of zero source and one destination) contributes nothing to the game's image, so
  // it is not sent to the ray tracer either. Only states the game has set are trusted.
  if (sims3cam::enabled() && g_sims3.held.kind == sims3cam::Kind::Main && primCount > 0) {
    auto& h = g_sims3;
    const DWORD* rs = m_state.renderStates.data();
    const bool cwOff = h.rsSet[D3DRS_COLORWRITEENABLE] && (rs[D3DRS_COLORWRITEENABLE] & 0xF) == 0;
    const bool zNever = h.rsSet[D3DRS_ZFUNC] && rs[D3DRS_ZFUNC] == D3DCMP_NEVER && (!h.rsSet[D3DRS_ZENABLE] || rs[D3DRS_ZENABLE] != D3DZB_FALSE);
    const bool aNever = h.rsSet[D3DRS_ALPHATESTENABLE] && rs[D3DRS_ALPHATESTENABLE] && h.rsSet[D3DRS_ALPHAFUNC] && rs[D3DRS_ALPHAFUNC] == D3DCMP_NEVER;
    const bool bZero = h.rsSet[D3DRS_ALPHABLENDENABLE] && rs[D3DRS_ALPHABLENDENABLE] && h.rsSet[D3DRS_SRCBLEND] && h.rsSet[D3DRS_DESTBLEND] && rs[D3DRS_SRCBLEND] == D3DBLEND_ZERO && rs[D3DRS_DESTBLEND] == D3DBLEND_ONE;
    if (cwOff || zNever || aNever || bZero) {
      ++h.invisibleDrawsSkipped;
      if (h.invisibleLogged < 6) {
        ++h.invisibleLogged;
        char msg[300];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: invisible draw skipped at frame %u -> VS %016llx PS %016llx: colour writes %lu, zfunc %lu, alpha test %lu/%lu, blend %lu %lu/%lu, %u primitives",
                 h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash,
                 (unsigned long) rs[D3DRS_COLORWRITEENABLE], (unsigned long) rs[D3DRS_ZFUNC], (unsigned long) rs[D3DRS_ALPHATESTENABLE], (unsigned long) rs[D3DRS_ALPHAFUNC],
                 (unsigned long) rs[D3DRS_ALPHABLENDENABLE], (unsigned long) rs[D3DRS_SRCBLEND], (unsigned long) rs[D3DRS_DESTBLEND], (unsigned) primCount);
        Logger::info(msg);
      }
      SIMS3_END_DRAW();
      return D3D_OK;
    }
  }
  UID currentUID = 0;
  {
    // The Sims 3 camera hook: a captured hardware-instanced draw goes to the runtime once per
    // instance (see Sims3Hook::drawCaptured), the instance streams' offsets stepping through
    // their data and stream 0 at one instance for the duration; the game's settings come back
    // afterwards.
    uint32_t instances_ = 1; uint32_t instStreams_ = 0;
    if (sims3cam::enabled() && g_sims3.drawCaptured && (m_state.streamFreqs[0] & D3DSTREAMSOURCE_INDEXEDDATA)) {
      instances_ = m_state.streamFreqs[0] & 0x3FFFFFFFu;
      for (uint32_t s_ = 1; s_ < caps::MaxStreams; ++s_)
        if ((m_state.streamFreqs[s_] & D3DSTREAMSOURCE_INSTANCEDATA) && *m_state.streams[s_] != nullptr && m_state.streamStrides[s_] > 0) instStreams_ |= 1u << s_;
      if (instances_ <= 1 || instances_ > 1024 || instStreams_ == 0) instances_ = 1;
    }
    if (instances_ > 1) {
      const UINT freq0_ = m_state.streamFreqs[0];
      IDirect3DVertexBuffer9* vb_[caps::MaxStreams] = {}; UINT off_[caps::MaxStreams] = {}, stride_[caps::MaxStreams] = {};
      for (uint32_t s_ = 1; s_ < caps::MaxStreams; ++s_) if (instStreams_ & (1u << s_)) {
        vb_[s_] = (IDirect3DVertexBuffer9*) bridge_cast<Direct3DVertexBuffer9_LSS*>(*m_state.streams[s_]);
        vb_[s_]->AddRef();   // held across the re-bindings below (each one drops the state's reference first)
        off_[s_] = m_state.streamOffsets[s_]; stride_[s_] = m_state.streamStrides[s_];
      }
      // The runtime folds draws of one frame with the same material, geometry and vertex-shader
      // hash into one object, and the vertex-shader hash of a captured draw covers the bytecode AND
      // the float constants the shader can reach (d3d9_rtx_geometry.cpp). A per-instance value in
      // c255 (never uploaded by the game; the variant bound for the draw reads it, see
      // appendConstantRead) keeps each instance its own object.
      float savedTag_[4]; memcpy(savedTag_, &m_state.vertexConstants.fConsts[255], sizeof savedTag_);
      SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 1u);
      for (uint32_t i_ = 0; i_ < instances_; ++i_) {
        for (uint32_t s_ = 1; s_ < caps::MaxStreams; ++s_) if (instStreams_ & (1u << s_)) SetStreamSource(s_, vb_[s_], off_[s_] + i_ * stride_[s_], stride_[s_]);
        const float tag_[4] = { (float) (i_ + 1), 0.f, 0.f, 0.f };
        SetVertexShaderConstantF(255, tag_, 1);
        ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, getId());
        currentUID = c.get_uid();
        c.send_many(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
      }
      SetVertexShaderConstantF(255, savedTag_, 1);
      for (uint32_t s_ = 1; s_ < caps::MaxStreams; ++s_) if (instStreams_ & (1u << s_)) { SetStreamSource(s_, vb_[s_], off_[s_], stride_[s_]); vb_[s_]->Release(); }
      SetStreamSourceFreq(0, freq0_);
      ++g_sims3.deinstancedDraws; g_sims3.deinstancedInstances += instances_;
      if (g_sims3.deinstanceLogged < 8) {
        ++g_sims3.deinstanceLogged; char msg[224];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: instanced draw split at frame %u -> %u instances, %u primitives each, instance streams %x, VS %016llx PS %016llx", g_sims3.frames + 1, instances_, (unsigned) primCount, instStreams_, (unsigned long long) g_sims3.vsHash, (unsigned long long) g_sims3.psHash);
        Logger::info(msg);
      }
    } else {
      // The Sims 3 camera hook: a captured wall draw gets its window and door openings cut into
      // the geometry (milestone 13, sims3_walls.h). The pixel shader's mask test is evaluated on
      // the client from the buffers' shadow copies and the mask atlas, and the cut triangles are
      // drawn from the hook's own buffers; the game's bindings come back afterwards. A draw whose
      // opening test cannot be evaluated -- a pixel shader the analyser does not know, walls C
      // without its alpha test, a mask not readable on the client -- still loses the triangles its
      // vertex shader hides, which never depended on the mask (milestone 77).
      bool wallDone_ = false;
      if (sims3cam::enabled() && g_sims3.drawCaptured && Type == D3DPT_TRIANGLELIST && g_sims3.wallLayout.valid && g_sims3.vsWall && g_sims3.vsWall->valid
          && *m_state.streams[0] != nullptr && *m_state.streams[1] != nullptr && *m_state.indices != nullptr) {
        auto wallDraw_ = [&]() -> bool {
          auto& h = g_sims3;
          const sims3cam::PsAnalysis* ps = (h.psAuto && h.psAuto->valid && h.psAuto->maskSampler >= 0 && h.psAuto->maskSampler < 16) ? h.psAuto : nullptr;
          const DWORD* rs = m_state.renderStates.data();
          // the discard threshold: texkill at 0.5; walls C alpha-tests mask + z - 0.5 against the
          // reference; below 0: no opening test
          float thr = -1.f;
          if (ps && ps->maskKill) thr = 0.5f;
          else if (ps && ps->maskAlpha && rs[D3DRS_ALPHATESTENABLE] && (rs[D3DRS_ALPHAFUNC] == D3DCMP_GREATEREQUAL || rs[D3DRS_ALPHAFUNC] == D3DCMP_GREATER)) thr = 0.5f + (float) (rs[D3DRS_ALPHAREF] & 0xFFu) / 255.f;
          auto* vb0 = bridge_cast<Direct3DVertexBuffer9_LSS*>(*m_state.streams[0]);
          auto* vb1 = bridge_cast<Direct3DVertexBuffer9_LSS*>(*m_state.streams[1]);
          auto* ib = bridge_cast<Direct3DIndexBuffer9_LSS*>(*m_state.indices);
          const uint8_t* d0 = vb0->sims3Data(); const uint8_t* d1 = vb1->sims3Data(); const uint8_t* di = ib->sims3Data();
          if (!d0 || !d1 || !di) { ++h.wallSkipped; return false; }
          // the mask: the texture at the pixel shader's mask sampler (none bound: black everywhere)
          const int s = ps ? ps->maskSampler : -1;
          uint32_t maskId = 0, maskVer = 0, maskW = 0, maskH = 0, maskFmt = 0; uint64_t maskHash = 0; const uint8_t* mask = nullptr;
          if (thr >= 0.f && h.boundTex[s] != nullptr) {
            if ((h.boundKind[s] & 0x7F) == 1) {
              auto* tex = bridge_cast<Direct3DTexture9_LSS*>(h.boundTex[s]);
              const D3DSURFACE_DESC d = tex->getLevelDesc(0);
              maskId = (uint32_t) tex->getId(); maskVer = tex->sims3Level0Version(); maskW = d.Width; maskH = d.Height; maskFmt = (uint32_t) d.Format;
              mask = sims3WallMask(h, maskId, maskVer, maskFmt, d.Width, d.Height, tex->sims3Level0Data(), maskHash);
            }
            if (!mask) {
              thr = -1.f;
              if (h.wallSkipLogged < 4) {
                ++h.wallSkipLogged; char fb[16]; char m[288];
                snprintf(m, sizeof m, "Sims 3 camera hook: wall draw's openings not cut at frame %u -> the mask at stage %d (%s %ux%u) is not readable on the client (its hidden triangles still go), VS %016llx PS %016llx", h.frames + 1, s, sims3FormatName(maskFmt, fb, sizeof fb), maskW, maskH, (unsigned long long) h.vsHash, (unsigned long long) h.psHash);
                Logger::info(m);
              }
            }
          }
          if (thr < 0.f) { maskW = maskH = 0; maskHash = 0; ++h.wallNoOpeningTest; }
          sims3cam::WallCutInput in;
          in.layout = h.wallLayout;
          in.vb0 = d0; in.vb0Size = vb0->sims3Size(); in.offset0 = m_state.streamOffsets[0]; in.stride0 = m_state.streamStrides[0];
          in.vb1 = d1; in.vb1Size = vb1->sims3Size(); in.offset1 = m_state.streamOffsets[1]; in.stride1 = m_state.streamStrides[1];
          in.ib = di; in.ibSize = ib->sims3Size(); in.ib32 = ib->getDesc().Format == D3DFMT_INDEX32;
          in.baseVertex = BaseVertexIndex; in.startIndex = startIndex; in.primCount = primCount;
          in.mask = mask; in.maskW = maskW; in.maskH = maskH;
          float ck[4]; memcpy(ck, &m_state.vertexConstants.fConsts[h.vsWall->clampReg], sizeof ck);
          in.params.clampLo = ck[0]; in.params.clampHi = ck[1]; in.params.clampVis = ck[2]; in.params.kScale = h.vsWall->kScale; in.params.threshold = thr;
          // the key follows the buffers' and the mask's CONTENT (hashed once per upload), the draw range, the
          // declaration and the constants the cut depends on
          struct { uint64_t vb0Hash, vb1Hash, ibHash, maskHash; uint32_t off0, st0, off1, st1, ib32, declId; int32_t base; uint32_t start, prims; float lo, hi, vis, kScale, thr; } k;
          memset(&k, 0, sizeof k);
          k.vb0Hash = sims3ContentHash(h, (uint32_t) vb0->getId(), vb0->sims3Version, d0, in.vb0Size);
          k.vb1Hash = sims3ContentHash(h, (uint32_t) vb1->getId(), vb1->sims3Version, d1, in.vb1Size);
          k.ibHash = sims3ContentHash(h, (uint32_t) ib->getId(), ib->sims3Version, di, in.ibSize);
          k.maskHash = maskHash; k.off0 = in.offset0; k.st0 = in.stride0; k.off1 = in.offset1; k.st1 = in.stride1; k.ib32 = in.ib32 ? 1u : 0u; k.declId = h.wallDeclId;
          k.base = BaseVertexIndex; k.start = startIndex; k.prims = primCount; k.lo = ck[0]; k.hi = ck[1]; k.vis = ck[2]; k.kScale = in.params.kScale; k.thr = thr;
          const uint64_t key = sims3cam::fnv1a64(&k, sizeof k);
          sims3cam::WallCutStats st; bool built = false;
          Sims3Hook::WallEntry* e = sims3WallEntry(h, this, key, in, st, built);
          ++h.wallDraws;
          if (built && st.hidden && h.wallHiddenLogged < 8) {
            ++h.wallHiddenLogged; char m[320];
            snprintf(m, sizeof m, "Sims 3 camera hook: wall draw with hidden vertices at frame %u -> VS %016llx PS %016llx: %u of %u triangles dropped (%u with mixed corners, %u hidden corners), %u cut, %u removed; cK = (%.2f, %.2f, %.2f)",
                     h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, st.hidden, (unsigned) primCount, st.hiddenMixed, st.hiddenCorners, st.cut, st.removed, ck[0], ck[1], ck[2]);
            Logger::info(m);
          }
          if (!e || !e->changed) return false;
          ++h.wallCutDraws;
          // the game's buffers are held by a reference of the hook's own while its own are bound
          IDirect3DVertexBuffer9* gvb0 = (IDirect3DVertexBuffer9*) vb0; IDirect3DVertexBuffer9* gvb1 = (IDirect3DVertexBuffer9*) vb1; IDirect3DIndexBuffer9* gib = (IDirect3DIndexBuffer9*) ib;
          gvb0->AddRef(); gvb1->AddRef(); gib->AddRef();
          const UINT off0 = m_state.streamOffsets[0], off1 = m_state.streamOffsets[1], st0 = m_state.streamStrides[0], st1 = m_state.streamStrides[1];
          SetStreamSource(0, e->vb0, 0, st0); SetStreamSource(1, e->vb1, 0, st1); SetIndices(e->ib);
          {
            ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, getId());
            currentUID = c.get_uid();
            const INT base0 = 0; const UINT min0 = 0, start0 = 0, nv = e->vertexCount, np = e->triangleCount;
            c.send_many(Type, base0, min0, nv, start0, np);
          }
          SetStreamSource(0, gvb0, off0, st0); SetStreamSource(1, gvb1, off1, st1); SetIndices(gib);
          gvb0->Release(); gvb1->Release(); gib->Release();
          if (built && h.wallLogged < 8) {
            ++h.wallLogged; char m[400];
            snprintf(m, sizeof m, "Sims 3 camera hook: wall openings cut at frame %u -> VS %016llx PS %016llx: %u triangles -> %u (%u cut, %u removed, %u hidden; %u cells, %u with openings, %u rectangles), %u vertices, mask s%d %ux%u%s, up-ness %.2f..%.2f x%.2f, visible from %.2f, threshold %.2f",
                     h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, (unsigned) primCount, e->triangleCount, st.cut, st.removed, st.hidden, st.cells, st.cellsWithOpenings, st.holeRects, e->vertexCount, s, maskW, maskH, thr < 0.f ? " (no opening test)" : mask ? "" : " (none bound: black)", ck[0], ck[1], in.params.kScale, ck[2], thr);
            Logger::info(m);
          }
          return true;
        };
        wallDone_ = wallDraw_();
      }
      // The Sims 3 camera hook: a low-detail lot model (milestones 69-71), drawn by the hook in its parts.
      if (!wallDone_ && sims3cam::enabled() && !g_sims3.ourDraw && (sims3cam::terrainLotPlate() || sims3cam::lotGlow()) && g_sims3.drawCaptured
          && g_sims3.vsHash == sims3cam::kLotImpostorVs && Type == D3DPT_TRIANGLELIST)
        wallDone_ = sims3LotModelDraw(g_sims3, this, m_state, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount, currentUID);
      if (!wallDone_ && sims3cam::enabled() && g_sims3.splitDraw && Type == D3DPT_TRIANGLELIST && primCount >= 2) {
        // The Sims 3 camera hook: a lot's re-submission as two half draws (milestone 17r), each its
        // own geometry to the runtime's draw tracker; together they bake the same triangles.
        const UINT half_ = primCount / 2;
        { ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, getId()); currentUID = c.get_uid(); c.send_many(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, half_); }
        { ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, getId()); currentUID = c.get_uid(); c.send_many(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex + half_ * 3, primCount - half_); }
        ++g_sims3.splitDraws;
      } else if (!wallDone_) {
        ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, getId());
        currentUID = c.get_uid();
        c.send_many(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
      }
    }
  }
  if (sims3cam::enabled() && g_sims3.compositeSecond) { SIMS3_END_DRAW(); sims3CompositeSecondPass(g_sims3, this, true, Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount); }
  else if (sims3cam::enabled() && g_sims3.mergePending >= 0 && !g_sims3.reissue) { SIMS3_END_DRAW(); sims3MergedSquareDraw(g_sims3, this); }   // the square's merged shape (milestone 60)
  else SIMS3_END_DRAW();
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("DrawIndexedPrimitive()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  ZoneScoped;
  LogFunctionCall();
  if (sims3cam::enabled()) g_sims3.drawIndexed = false;
  SIMS3_BEGIN_DRAW();
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_DrawPrimitiveUP, getId());
    currentUID = c.get_uid();
    c.send_many(PrimitiveType, PrimitiveCount);

    uint32_t numIndices = GetIndexCount(PrimitiveType, PrimitiveCount);
    uint32_t vertexDataSize = numIndices * VertexStreamZeroStride;

    c.send_data(vertexDataSize, (void*) pVertexStreamZeroData);
    c.send_data(VertexStreamZeroStride);
  }
  SIMS3_END_DRAW();
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("DrawPrimitiveUP()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  ZoneScoped;
  LogFunctionCall();
  if (sims3cam::enabled()) g_sims3.drawIndexed = false;
  SIMS3_BEGIN_DRAW();
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitiveUP, getId());
    currentUID = c.get_uid();
    c.send_many(PrimitiveType, MinIndex, NumVertices, PrimitiveCount, IndexDataFormat, VertexStreamZeroStride);

    uint32_t numIndices = GetIndexCount(PrimitiveType, PrimitiveCount);
    uint32_t indexStride = IndexDataFormat == D3DFMT_INDEX16 ? 2 : 4;
    uint32_t indexDataSize = numIndices * indexStride;
    uint32_t vertexDataSize = NumVertices * VertexStreamZeroStride;

    c.send_data(indexDataSize, (void*) pIndexData);
    c.send_data(vertexDataSize, (void*) pVertexStreamZeroData);
  }
  SIMS3_END_DRAW();
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("DrawIndexedPrimitiveUP()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) {
  ZoneScoped;
  LogMissingFunctionCall();

  if (pDestBuffer == nullptr || pVertexDecl == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  auto* const pLssVtxDecl = bridge_cast<Direct3DVertexDeclaration9_LSS*>(pVertexDecl);
  const UID vtxDeclId = (pLssVtxDecl) ? (UID) pLssVtxDecl->getId() : 0;

  auto* const pLssDestBuffer = bridge_cast<Direct3DVertexBuffer9_LSS*>(pDestBuffer);
  const UID destBufferId = (pLssDestBuffer) ? (UID) pLssDestBuffer->getId() : 0;

  // Send command to server and wait for response
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_ProcessVertices, getId());
    currentUID = c.get_uid();
    c.send_many(SrcStartIndex, DestIndex, VertexCount);
    c.send_data(destBufferId);
    c.send_data(vtxDeclId);
    c.send_data(Flags);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("ProcessVertices()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) {
  ZoneScoped;
  LogFunctionCall();
  if (pVertexElements == nullptr || ppDecl == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  UID currentUID = 0;
  {
    auto* const pLssVtxDecl = trackWrapper(new Direct3DVertexDeclaration9_LSS(this, pVertexElements));
    (*ppDecl) = pLssVtxDecl;

    size_t numElem = 1; // We add one so we send the end marker as well
    const auto pStart = pVertexElements;
    const auto* pVtxElemItr = pVertexElements;
    while (pVtxElemItr->Stream != 0xFF) {
      numElem++;
      pVtxElemItr++;
    }

    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateVertexDeclaration, getId());
      currentUID = c.get_uid();
      c.send_data(numElem);
      c.send_data(sizeof(D3DVERTEXELEMENT9) * numElem, (void*) pStart);
      c.send_data((uint32_t) pLssVtxDecl->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateVertexDeclaration()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) {
  ZoneScoped;
  LogFunctionCall();

  auto* const pLssVtxDecl = bridge_cast<Direct3DVertexDeclaration9_LSS*>(pDecl);
  const UID id = (pLssVtxDecl) ? (UID) pLssVtxDecl->getId() : 0;
  if (sims3cam::enabled()) sims3NoteDecl(g_sims3, pDecl);
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      m_state.vertexDecl = MakeD3DAutoPtr(pLssVtxDecl);
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetVertexDeclaration, getId());
      currentUID = c.get_uid();
      c.send_data(id);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetVertexDeclaration()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) {
  ZoneScoped;
  LogFunctionCall();

  if (ppDecl == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    auto* const pLssVertexDecl = bridge_cast<Direct3DVertexDeclaration9_LSS*>(*m_state.vertexDecl);
    *ppDecl = (IDirect3DVertexDeclaration9*) pLssVertexDecl;
    if ((*ppDecl)) {
      (*ppDecl)->AddRef();
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetFVF(DWORD FVF) {
  ZoneScoped;
  LogFunctionCall();
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      m_FVF = FVF;
      if (sims3cam::enabled()) { g_sims3.declIs3D = sims3cam::fvfIs3D(FVF); g_sims3.wallLayout = sims3cam::WallLayout(); g_sims3.wallDeclId = 0; }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetFVF, getId());
      currentUID = c.get_uid();
      c.send_data(FVF);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetFVF()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetFVF(DWORD* pFVF) {
  ZoneScoped;
  LogFunctionCall();

  if (pFVF == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pFVF = m_FVF;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) {
  ZoneScoped;
  LogFunctionCall();

  if (ppShader == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  if (m_caps.VertexShaderVersion == D3DVS_VERSION(0, 0))
    return D3DERR_INVALIDCALL;

  // The Sims 3 camera hook: shaders are recognised by the hash of the ORIGINAL bytecode.
  // Some get their diffuse texcoord promoted to TEXCOORD0 (see kTexcoordPromotes); the
  // patched copy is what both the wrapper and the runtime receive.
  const DWORD* function = pFunction;
  std::vector<DWORD> sims3Patched;
  const sims3cam::ShaderPatch* sims3ConstPatch = nullptr;
  const bool sims3Ours = g_sims3.creatingVariant;   // a promoted variant of the game's shader, made by the hook: no tables, no dump
  const bool sims3Hooked = sims3cam::enabled() && !sims3Ours;
  const size_t sims3Count = sims3Hooked ? sims3cam::shaderTokenCount(pFunction) : 0;   // 0: no END token, the analyses refuse the stream
  const uint64_t sims3Hash = sims3Count ? sims3cam::fnv1a64(pFunction, sims3Count * sizeof(DWORD)) : 0;
  if (sims3Hooked) {
    sims3ConstPatch = sims3cam::findShaderPatch(sims3Hash);
    if (const sims3cam::TexcoordPromote* promote = sims3cam::findTexcoordPromote(sims3Hash)) {
      const size_t n = sims3Count;
      sims3Patched.assign(pFunction, pFunction + n);
      const uint32_t changed = sims3cam::promoteTexcoord(sims3Patched.data(), n, promote->texcoordIndex);
      char msg[224];
      if (changed) {
        function = sims3Patched.data();
        snprintf(msg, sizeof msg, "Sims 3 camera hook: texcoord %u promoted to TEXCOORD0 (%u tokens) -> %s", (unsigned) promote->texcoordIndex, changed, promote->name);
      } else {
        snprintf(msg, sizeof msg, "Sims 3 camera hook: texcoord promotion matched nothing in the bytecode -> %s (left unpatched)", promote->name);
      }
      Logger::info(msg);
    }
  }

  CommonShader shader(function);
  if (D3DSHADER_VERSION_MAJOR(m_caps.VertexShaderVersion) < shader.getMajorVersion())
    return D3DERR_INVALIDCALL;

  auto* const pLssVertexShader = trackWrapper(new Direct3DVertexShader9_LSS(this, shader));
  (*ppShader) = pLssVertexShader;
  if (sims3Hooked) {
    pLssVertexShader->sims3Patch = sims3ConstPatch;
    pLssVertexShader->sims3Hash = sims3Hash;
    sims3DumpShader("vs", sims3Hash, pFunction, sims3Count);
    if (pLssVertexShader->sims3Patch) {
      char msg[192];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: shader recognised -> %s", pLssVertexShader->sims3Patch->name);
      Logger::info(msg);
    }
    if (const sims3cam::NeverCapture* n = sims3cam::findNeverCapture(pLssVertexShader->sims3Hash)) {
      pLssVertexShader->sims3NeverCapture = sims3cam::neverCaptureMode(n);
      char msg[224];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: %s of this shader are never captured -> %s", n->blendedOnly ? "alpha-blended draws" : "draws", n->name);
      Logger::info(msg);
    } else if (sims3cam::isSkyDomeShader(pFunction, sims3Count)) {
      pLssVertexShader->sims3SkyDome = true;
      char msg[224];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: vertex shader %016llx pins its position's z to w (a sky dome) -> its draws are presented to the runtime as the sky", (unsigned long long) sims3Hash);
      Logger::info(msg);
    }
    // where the world-space normal leaves the shader (milestone 11), from the original bytecode
    sims3cam::analyzeVertexNormal(pFunction, sims3Count, pLssVertexShader->sims3Normal);
    if (pLssVertexShader->sims3Normal.hasNormalInput && g_sims3.normalShaderLogged < 80) {
      ++g_sims3.normalShaderLogged;
      const sims3cam::VsNormalInfo& ni = pLssVertexShader->sims3Normal;
      char list[128] = {}; int ln = 0;
      for (int i = 0; i < 16 && ln < (int) sizeof list - 20; ++i) if (ni.candidates & (1u << i)) ln += snprintf(list + ln, sizeof list - ln, "%sTEXCOORD%d (o%u)", ln ? ", " : "", i, (unsigned) ni.outReg[i]);
      char msg[288];
      if (ni.candidates) snprintf(msg, sizeof msg, "Sims 3 camera hook: vertex shader %016llx (vs_%u_0): world normal output %s", (unsigned long long) pLssVertexShader->sims3Hash, (unsigned) ni.version, list);
      else snprintf(msg, sizeof msg, "Sims 3 camera hook: vertex shader %016llx (vs_%u_0): normal input but no output derived from it alone (input hidden from the capture)", (unsigned long long) pLssVertexShader->sims3Hash, (unsigned) ni.version);
      Logger::info(msg);
    }
    // a wall shader (milestone 13): the clamp of the up-ness flag, for the opening cut
    if (sims3cam::analyzeWallVertexShader(pFunction, sims3Count, pLssVertexShader->sims3Wall)) {
      char msg[224];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: vertex shader %016llx is a wall shader (opening test; up-ness clamp at c%u, scale %.3f)", (unsigned long long) pLssVertexShader->sims3Hash, (unsigned) pLssVertexShader->sims3Wall.clampReg, pLssVertexShader->sims3Wall.kScale);
      Logger::info(msg);
    }
  }

  uint32_t dataSize = 0;
  pLssVertexShader->GetFunction(nullptr, &dataSize);

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateVertexShader, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) pLssVertexShader->getId());
    c.send_data(dataSize);
    c.send_data(dataSize, (void*) function);
    
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateVertexShader()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetVertexShader(IDirect3DVertexShader9* pShader) {
  ZoneScoped;
  LogFunctionCall();

  // NULL is an allowed value for pShader
  auto* const pLssVertexShader = bridge_cast<Direct3DVertexShader9_LSS*>(pShader);
  const auto id = (pLssVertexShader) ? (uint32_t) pLssVertexShader->getId() : 0;
  if (sims3cam::enabled() && !g_sims3.swappingVs && !m_stateRecording) sims3NoteVertexShader(g_sims3, pShader);   // our own variant swap around a draw leaves the game's facts in place; a shader set while a state block records does not reach the device
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.vertexShader = MakeD3DAutoPtr(pLssVertexShader);
        m_stateRecording->m_dirtyFlags.vertexShader = true;
      } else {
        m_state.vertexShader = MakeD3DAutoPtr(pLssVertexShader);
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetVertexShader, getId());
      currentUID = c.get_uid();
      c.send_data(id);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetVertexShader()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetVertexShader(IDirect3DVertexShader9** ppShader) {
  ZoneScoped;
  LogFunctionCall();

  if (ppShader == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    auto pLssVertexShader = bridge_cast<Direct3DVertexShader9_LSS*>(*m_state.vertexShader);
    (*ppShader) = (IDirect3DVertexShader9*) pLssVertexShader;
    if ((*ppShader)) {
      (*ppShader)->AddRef();
    }
  }
  return S_OK;
}

using ShaderType = BaseDirect3DDevice9Ex_LSS::ShaderConstants::ShaderType;
using ConstantType = BaseDirect3DDevice9Ex_LSS::ShaderConstants::ConstantType;

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // The Sims 3 camera hook: the World rows c4..c6 as the device holds them, for the lot terrain's
  // per-frame copy key (milestone 16); an upload may cover them partly.
  if (sims3cam::enabled() && !g_sims3.ourConsts && !m_stateRecording) {
    const UINT first = StartRegister > 4 ? StartRegister : 4, last = (StartRegister + Vector4fCount < 7) ? StartRegister + Vector4fCount : 7;
    for (UINT r = first; r < last; ++r) memcpy(g_sims3.rows4to6 + (r - 4) * 4, pConstantData + (r - StartRegister) * 4, 4 * sizeof(float));
  }
  // The Sims 3 camera hook: rewrite the constants that make the bound shader hide geometry
  // (see kShaderPatches in sims3_camera_hook.h), so every vertex reaches Remix as a point.
  float patchedData[256 * 4];
  const float* data = pConstantData;
  if (sims3cam::enabled() && g_sims3.patch && Vector4fCount <= 256 &&
      sims3cam::patchIntersects(g_sims3.patch, StartRegister, Vector4fCount)) {
    memcpy(patchedData, pConstantData, Vector4fCount * 4 * sizeof(float));
    sims3cam::applyPatches(g_sims3.patch, StartRegister, patchedData, Vector4fCount);
    data = patchedData;
    const uint32_t bit = 1u << (uint32_t) (g_sims3.patch - sims3cam::kShaderPatches);
    if (!(g_sims3.loggedPatches & bit)) {
      g_sims3.loggedPatches |= bit;
      char msg[192];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: constants patched in flight -> %s", g_sims3.patch->name);
      Logger::info(msg);
    }
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult =
      setShaderConstants<
      ShaderType::Vertex,
      ConstantType::Float>(
        StartRegister,
        data,
        Vector4fCount);
  }
  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetVertexShaderConstantF,
                   StartRegister,
                   data,
                   Vector4fCount,
                   Vector4fCount * 4 * sizeof(float), currentUID);

    // The Sims 3 camera hook: the game never calls SetTransform, so recover View and
    // Projection from the fused matrix it uploads at c0 and forward them; without this
    // the Remix runtime finds no camera and captures no geometry. See sims3_camera_hook.h.
    // (Must precede WAIT_FOR_OPTIONAL_SERVER_RESPONSE, which returns unconditionally.)
    if (StartRegister == 0 && Vector4fCount >= 4 && sims3cam::enabled() && !g_sims3.ourConsts && !m_stateRecording) {
      // Maintain the verified main camera; the transforms themselves are applied per draw
      // (sims3ApplyForDraw), where the depth-test state and vertex layout are known.
      auto& h = g_sims3;
      sims3cam::Camera cam;
      const sims3cam::Kind kind = sims3cam::classify(pConstantData, Vector4fCount, cam);
      char msg[256];
      if (kind == sims3cam::Kind::Main) {
        // the play camera does not move within a frame: a main camera unlike the frame's first one
        // (lens or position) is not adopted (run 70: the once-per-frame origin camera at a horizon tilt)
        bool adopt = true;
        if (!h.frameCamSet) { h.frameCam = cam; h.frameCamSet = true; }
        else {
          const sims3cam::Camera& f = h.frameCam;
          const float dp[3] = { cam.pos[0] - f.pos[0], cam.pos[1] - f.pos[1], cam.pos[2] - f.pos[2] };
          // the same camera re-derived from another object's fused matrix agrees to ~1e-6 relative;
          // anything beyond is another lens (near / far plane), position or direction
          const bool differs = std::fabs(cam.fovY - f.fovY) > 1e-4f || std::fabs(cam.nearZ - f.nearZ) > 1e-3f * f.nearZ || std::fabs(cam.aspect - f.aspect) > 1e-3f
                            || std::fabs(cam.proj._33 - f.proj._33) > 2e-6f || std::fabs(cam.proj._43 - f.proj._43) > 1e-4f * std::fabs(f.proj._43) + 1e-5f
                            || sims3cam::len3(dp) > 0.05f || sims3cam::dot3(cam.fwd, f.fwd) < 0.99999f;
          if (differs) adopt = false;
        }
        if (adopt) { h.cam = cam; h.cameraValid = true; h.camMirrored = false; }
        if (!h.loggedMain) {
          h.loggedMain = true;
          snprintf(msg, sizeof msg, "Sims 3 camera hook: main camera verified (fovY=%.1f deg, aspect=%.3f, near=%.4f, P33=%.7f P43=%.5f, eye=%.1f,%.1f,%.1f)",
                   cam.fovY * 57.2958f, cam.aspect, cam.nearZ, cam.proj._33, cam.proj._43, cam.pos[0], cam.pos[1], cam.pos[2]);
          Logger::info(msg);
        }
      } else if (kind == sims3cam::Kind::OtherCamera) {
        h.cameraValid = false; h.camMirrored = true;   // a reflection pass: its 3D draws are dropped
        ++h.mirroredUploads;
        if (!h.loggedOther) {
          h.loggedOther = true;
          snprintf(msg, sizeof msg, "Sims 3 camera hook: reflection camera (mirrored basis; eye=%.1f,%.1f,%.1f, fwd.y=%.2f) at frame %u -> its draws are dropped", cam.pos[0], cam.pos[1], cam.pos[2], cam.fwd[1], h.frames + 1);
          Logger::info(msg);
        }
        if (cam.fwd[1] < -0.05f && !h.loggedMirrorCam) {
          h.loggedMirrorCam = true;
          snprintf(msg, sizeof msg, "Sims 3 camera hook: reflection camera looking down at frame %u (a wall mirror's pass; eye=%.1f,%.1f,%.1f, fwd=%.2f,%.2f,%.2f) -> its draws are dropped", h.frames + 1, cam.pos[0], cam.pos[1], cam.pos[2], cam.fwd[0], cam.fwd[1], cam.fwd[2]);
          Logger::info(msg);
        }
      }
    }

    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetVertexShaderConstantF()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<
      ShaderType::Vertex,
      ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = setShaderConstants<
      ShaderType::Vertex,
      ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }
  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetVertexShaderConstantI,
                   StartRegister,
                   pConstantData,
                   Vector4iCount,
                   Vector4iCount * 4 * sizeof(UINT), currentUID);
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetVertexShaderConstantI()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<
      ShaderType::Vertex,
      ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }

  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = setShaderConstants<
      ShaderType::Vertex,
      ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }

  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetVertexShaderConstantB,
                   StartRegister,
                   pConstantData,
                   BoolCount,
                   BoolCount * sizeof(BOOL), currentUID);
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetVertexShaderConstantB()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<
      ShaderType::Vertex,
      ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) {
  ZoneScoped;
  LogFunctionCall();
  auto* const pLssStreamData = bridge_cast<Direct3DVertexBuffer9_LSS*>(pStreamData);
  const UID id = (pStreamData) ? (UID) pLssStreamData->getId() : 0;
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.streams[StreamNumber] = MakeD3DAutoPtr(pLssStreamData);
        if (pStreamData != nullptr) {
          m_stateRecording->m_captureState.streamOffsets[StreamNumber] = OffsetInBytes;
          m_stateRecording->m_captureState.streamStrides[StreamNumber] = Stride;
          m_stateRecording->m_dirtyFlags.streamOffsetsAndStrides[StreamNumber] = true;
        }
        m_stateRecording->m_dirtyFlags.streams[StreamNumber] = true;
      } else {
        m_state.streams[StreamNumber] = MakeD3DAutoPtr(pLssStreamData);
        if (pStreamData != nullptr) {
          m_state.streamOffsets[StreamNumber] = OffsetInBytes;
          m_state.streamStrides[StreamNumber] = Stride;
        }
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetStreamSource, getId());
      currentUID = c.get_uid();
      c.send_many(StreamNumber, id, OffsetInBytes, Stride);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetStreamSource()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) {
  ZoneScoped;
  LogFunctionCall();

  if (ppStreamData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    auto* pLssVertexBuffer = bridge_cast<Direct3DVertexBuffer9_LSS*>(*m_state.streams[StreamNumber]);
    (*ppStreamData) = (IDirect3DVertexBuffer9*) pLssVertexBuffer;
    (*pOffsetInBytes) = m_state.streamOffsets[StreamNumber];
    (*pStride) = m_state.streamStrides[StreamNumber];
    if ((*ppStreamData)) {
      (*ppStreamData)->AddRef();
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetStreamSourceFreq(UINT StreamNumber, UINT Divider) {
  ZoneScoped;
  LogFunctionCall();
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.streamFreqs[StreamNumber] = Divider;
        m_stateRecording->m_dirtyFlags.streamFreqs[StreamNumber] = true;
      } else {
        m_state.streamFreqs[StreamNumber] = Divider;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetStreamSourceFreq, getId());
      currentUID = c.get_uid();
      c.send_many(StreamNumber, Divider);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetStreamSourceFreq()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetStreamSourceFreq(UINT StreamNumber, UINT* Divider) {
  ZoneScoped;
  LogFunctionCall();
  if (Divider == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  {
    BRIDGE_DEVICE_LOCKGUARD();
    *Divider = m_state.streamFreqs[StreamNumber];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetIndices(IDirect3DIndexBuffer9* pIndexData) {
  ZoneScoped;
  LogFunctionCall();
  auto* const pLssIndexData = bridge_cast<Direct3DIndexBuffer9_LSS*>(pIndexData);
  const UID id = (pLssIndexData) ? (UID) pLssIndexData->getId() : 0;
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.indices = MakeD3DAutoPtr(pLssIndexData);
        m_stateRecording->m_dirtyFlags.indices = true;
      } else {
        m_state.indices = MakeD3DAutoPtr(pLssIndexData);
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetIndices, getId());
      currentUID = c.get_uid();
      c.send_data(id);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetIndices()", D3DERR_INVALIDCALL, currentUID);
} 

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetIndices(IDirect3DIndexBuffer9** ppIndexData) {
  ZoneScoped;
  LogFunctionCall();

  if (ppIndexData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    auto* pLssIndexBuffer = bridge_cast<Direct3DIndexBuffer9_LSS*>(*m_state.indices);
    (*ppIndexData) = (IDirect3DIndexBuffer9*) pLssIndexBuffer;
    if ((*ppIndexData)) {
      (*ppIndexData)->AddRef();
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) {
  ZoneScoped;
  LogFunctionCall();
 
  if(ppShader == NULL)
    return D3DERR_INVALIDCALL;

  (*ppShader) = NULL;

  if (ppShader == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  if (m_caps.PixelShaderVersion == D3DPS_VERSION(0, 0))
    return D3DERR_INVALIDCALL;

  CommonShader shader(pFunction);
  if (D3DSHADER_VERSION_MAJOR(m_caps.PixelShaderVersion) < shader.getMajorVersion())
    return D3DERR_INVALIDCALL;

  auto* const pLssPixelShader = trackWrapper(new Direct3DPixelShader9_LSS(this, shader));
  (*ppShader) = pLssPixelShader;
  pLssPixelShader->sims3Major = (uint8_t) shader.getMajorVersion();
  if (sims3cam::enabled() && !g_sims3.creatingVariant) {   // a terrain variant made by the hook: no tables, no dump
    const size_t count = sims3cam::shaderTokenCount(pFunction);   // 0: no END token, the analysis refuses the stream
    const uint64_t hash = count ? sims3cam::fnv1a64(pFunction, count * sizeof(DWORD)) : 0;
    pLssPixelShader->sims3Hash = hash;
    sims3DumpShader("ps", hash, pFunction, count);
    if (const sims3cam::AlbedoStage* a = sims3cam::findAlbedoStage(hash)) {
      pLssPixelShader->sims3AlbedoStage = a->stage;
      pLssPixelShader->sims3TintReg = a->tintReg;
      char tintNote[64] = "", msg[256];
      if (a->tintReg >= 0) snprintf(tintNote, sizeof tintNote, "; tint constant c%d forwarded as texture factor", (int) a->tintReg);
      snprintf(msg, sizeof msg, "Sims 3 camera hook: albedo is texture stage %u for %s%s", (unsigned) a->stage, a->name, tintNote);
      Logger::info(msg);
    }
    // what the bytecode says about its samplers: the albedo choice for untabled shaders, the
    // coordinate for tabled ones drawn with an untabled vertex shader, the wall opening mask
    sims3cam::analyzePixelShader(pFunction, count, pLssPixelShader->sims3Auto);
    if (pLssPixelShader->sims3Auto.cutSampler >= 0) ++g_sims3.cutShaders;
  }

  uint32_t dataSize = 0;
  pLssPixelShader->GetFunction(nullptr, &dataSize);

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreatePixelShader, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) pLssPixelShader->getId());
    c.send_data(dataSize);
    c.send_data(dataSize, (void*) pFunction);
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreatePixelShader()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetPixelShader(IDirect3DPixelShader9* pShader) {
  ZoneScoped;
  LogFunctionCall();
  Direct3DPixelShader9_LSS* pLssPixelShader = bridge_cast<Direct3DPixelShader9_LSS*>(pShader);
  const auto id = (pLssPixelShader) ? (uint32_t) pLssPixelShader->getId() : 0;
  if (sims3cam::enabled() && !g_sims3.swappingPs && !m_stateRecording) sims3NotePixelShader(g_sims3, pShader);   // our own variant swap around a terrain draw leaves the game's facts in place; not while a state block records
  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (m_stateRecording) {
      m_stateRecording->m_captureState.pixelShader = MakeD3DAutoPtr(pLssPixelShader);
      m_stateRecording->m_dirtyFlags.pixelShader = true;
    } else {
      m_state.pixelShader = MakeD3DAutoPtr(pLssPixelShader);
    }
  }
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetPixelShader, getId());
    currentUID = c.get_uid();
    c.send_data(id);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetPixelShader()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetPixelShader(IDirect3DPixelShader9** ppShader) {
  ZoneScoped;
  LogFunctionCall();

  if (ppShader == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    auto pLssPixelShader = bridge_cast<Direct3DPixelShader9_LSS*>(*m_state.pixelShader);
    (*ppShader) = (IDirect3DPixelShader9*) pLssPixelShader;
    if ((*ppShader)) {
      (*ppShader)->AddRef();
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = setShaderConstants<ShaderType::Pixel, ConstantType::Float>(StartRegister, pConstantData, Vector4fCount);
  }

  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetPixelShaderConstantF,
                   StartRegister,
                   pConstantData,
                   Vector4fCount,
                   Vector4fCount * 4 * sizeof(float), currentUID);
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetPixelShaderConstantF()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<ShaderType::Pixel, ConstantType::Float>(StartRegister, pConstantData, Vector4fCount);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = setShaderConstants<ShaderType::Pixel, ConstantType::Int>(StartRegister, pConstantData, Vector4iCount);
  }
  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetPixelShaderConstantI,
                   StartRegister,
                   pConstantData,
                   Vector4iCount,
                   Vector4iCount * 4 * sizeof(UINT), currentUID);
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetPixelShaderConstantI()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) {
  LogFunctionCall();

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<ShaderType::Pixel, ConstantType::Int>(StartRegister, pConstantData, Vector4iCount);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = setShaderConstants<ShaderType::Pixel, ConstantType::Bool>(StartRegister, pConstantData, BoolCount);
  }
  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetPixelShaderConstantB,
                   StartRegister,
                   pConstantData,
                   BoolCount,
                   BoolCount * sizeof(BOOL), currentUID);
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetPixelShaderConstantB()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) {
  ZoneScoped;
  LogFunctionCall();

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<ShaderType::Pixel, ConstantType::Bool>(StartRegister, pConstantData, BoolCount);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo) {
  ZoneScoped;
  LogMissingFunctionCall();
  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo) {
  ZoneScoped;
  LogMissingFunctionCall();
  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DeletePatch(UINT Handle) {
  ZoneScoped;
  LogMissingFunctionCall();
  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) {
  ZoneScoped;
  LogFunctionCall();
  // MSDN: This parameter can be set to NULL to see if a query is supported. 
  if (nullptr == ppQuery) {
    return S_OK;
  }

  auto* const pLssQuery = trackWrapper(new Direct3DQuery9_LSS(this, Type));
  (*ppQuery) = pLssQuery;

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateQuery, getId());
    currentUID = c.get_uid();
    c.send_many(Type, (uint32_t) pLssQuery->getId());
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetConvolutionMonoKernel(UINT width, UINT height, float* rows, float* columns) {
  ZoneScoped;
  LogFunctionCall();

  UID currentUID = 0;
  // Send command to server and wait for response
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetConvolutionMonoKernel, getId());
    currentUID = c.get_uid();
    c.send_data(width);
    c.send_data(height);
    c.send_data(sizeof(float) * width, (void*) rows);
    c.send_data(sizeof(float) * height, (void*) columns);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetConvolutionMonoKernel()", E_FAIL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ComposeRects(IDirect3DSurface9* pSrc, IDirect3DSurface9* pDst, IDirect3DVertexBuffer9* pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9* pDstRectDescs, D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset) {
  ZoneScoped;
  LogFunctionCall();

  // Send command to server and wait for response
  {
    const auto pLssSourceSurface = bridge_cast<Direct3DSurface9_LSS*>(pSrc);
    const auto pLssDestinationSurface = bridge_cast<Direct3DSurface9_LSS*>(pDst);

    auto* const pLssSrcRect = bridge_cast<Direct3DVertexBuffer9_LSS*>(pSrcRectDescs);
    const UID idSrcRect = (pSrcRectDescs) ? (UID) pLssSrcRect->getId() : 0;

    auto* const pLssDestRect = bridge_cast<Direct3DVertexBuffer9_LSS*>(pDstRectDescs);
    const UID idDestRect = (pDstRectDescs) ? (UID) pLssDestRect->getId() : 0;

    if (pLssSourceSurface && pLssDestinationSurface) {
      UID currentUID = 0;
      {
        ClientMessage c(Commands::IDirect3DDevice9Ex_ComposeRects, getId());
        currentUID = c.get_uid();
        c.send_many(pLssSourceSurface->getId(), pLssDestinationSurface->getId(), idSrcRect, idDestRect, NumRects, Operation, Xoffset, Yoffset);
      }
      WAIT_FOR_OPTIONAL_SERVER_RESPONSE("ComposeRects()", D3DERR_INVALIDCALL, currentUID);
    }
  }
  return D3DERR_INVALIDCALL;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::PresentEx(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion, DWORD dwFlags) {
  ZoneScoped;
  LogMissingFunctionCall();
  assert(m_ex);

  // If the bridge was disabled in the meantime for some reason we want to bail
  // out here so we don't spend time waiting on the Present semaphore or trying
  // to send keyboard state to the server.
  if (!gbBridgeRunning) {
    return D3D_OK;
  }

  return m_pSwapchain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetGPUThreadPriority(INT* pPriority) {
  ZoneScoped;
  LogFunctionCall();

  // Function does not claim to support returning D3DERR_INVALIDCALL
  if (pPriority == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pPriority = m_gpuThreadPriority;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetGPUThreadPriority(INT Priority) {
  ZoneScoped;
  LogFunctionCall();
  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_gpuThreadPriority = Priority;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::WaitForVBlank(UINT iSwapChain) {
  ZoneScoped;
  LogMissingFunctionCall();

  // This API always returns D3D_OK
  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources) {
  ZoneScoped;
  LogMissingFunctionCall();

  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetMaximumFrameLatency(UINT MaxLatency) {
  ZoneScoped;
  LogFunctionCall();
  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_maxFrameLatency = MaxLatency;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetMaximumFrameLatency(UINT* pMaxLatency) {
  ZoneScoped;
  LogFunctionCall();

  if (pMaxLatency == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pMaxLatency = m_maxFrameLatency;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CheckDeviceState(HWND hDestinationWindow) {
  ZoneScoped;
  LogFunctionCall();

  UID currentUID = 0;
  // Send command to server and wait for response
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CheckDeviceState, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) hDestinationWindow);
  }
  WAIT_FOR_SERVER_RESPONSE("CheckDeviceState()", E_FAIL, currentUID);
  HRESULT res = (HRESULT) DeviceBridge::get_data();
  DeviceBridge::pop_front();

  return res;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) {
  ZoneScoped;
  assert(m_ex);
  LogFunctionCall();

  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Format = Format;
  desc.MultiSampleType = MultiSample;
  desc.MultiSampleQuality = MultisampleQuality;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.Type = D3DRTYPE_SURFACE;

  // Insert our own IDirect3DSurface9 interface implementation
  Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
  (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

  UID currentUID = 0;
  {
    // Add a handle for the surface
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateRenderTargetEx, getId());
    currentUID = c.get_uid();
    c.send_many(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, Usage, pLssSurface->getId());
  }

  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateRenderTargetEx()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) {
  ZoneScoped;
  assert(m_ex);
  LogFunctionCall();

  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Format = Format;
  desc.MultiSampleType = D3DMULTISAMPLE_NONE;
  desc.MultiSampleQuality = 0;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = Pool;
  desc.Type = D3DRTYPE_SURFACE;

  // Insert our own IDirect3DSurface9 interface implementation
  Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
  (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

  UID currentUID = 0;
  {
    // Add a handle for the surface
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateOffscreenPlainSurfaceEx, getId());
    currentUID = c.get_uid();
    c.send_many(Width, Height, Format, Pool, Usage, pLssSurface->getId());
  }

  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateOffscreenPlainSurfaceEx()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) {
  ZoneScoped;
  assert(m_ex);
  LogFunctionCall();

  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Format = Format;
  desc.MultiSampleType = MultiSample;
  desc.MultiSampleQuality = MultisampleQuality;
  desc.Usage = D3DUSAGE_DEPTHSTENCIL;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.Type = D3DRTYPE_SURFACE;

  Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
  (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx, getId());
    currentUID = c.get_uid();
    c.send_many(Width, Height, Format, MultiSample, MultisampleQuality, Discard, Usage, pLssSurface->getId());
  }

  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateDepthStencilSurfaceEx()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ResetEx(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode) {
  ZoneScoped;
  assert(m_ex);
  LogFunctionCall();
  HRESULT res = S_OK;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    // Clear all device state and release implicit/internal objects
    releaseInternalObjects(false);
    
    const auto presParam = Direct3DSwapChain9_LSS::sanitizePresentationParameters(*pPresentationParameters, getCreateParams());
    m_presParams = presParam;
    WndProc::unset();
    WndProc::set(getWinProcHwnd());
    // Tell Server to do the Reset
    size_t currentUID = 0;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_ResetEx, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(D3DPRESENT_PARAMETERS), &presParam);
      c.send_data(sizeof(D3DDISPLAYMODEEX), pFullscreenDisplayMode);
    }

    // Perform an WAIT_FOR_OPTIONAL_SERVER_RESPONSE but don't return since we still have work to do.
    if (GlobalOptions::getSendAllServerResponses()) {
      const uint32_t timeoutMs = GlobalOptions::getAckTimeout();
      if (Result::Success != DeviceBridge::waitForCommand(Commands::Bridge_Response, timeoutMs, nullptr, true, currentUID)) {
        Logger::err("Direct3DDevice9Ex_LSS::ResetEx() failed with : no response from server.");
      }
      res = (HRESULT) DeviceBridge::get_data();
      DeviceBridge::pop_front();
    }

    // Reset swapchain and link server backbuffer/depth buffer after the server reset its swapchain, or we will link to the old backbuffer/depth resources
    initImplicitObjects(presParam);
    // Keeping a track of previous present parameters, to detect and handle mode changes
    m_previousPresentParams = *pPresentationParameters;
  }
  return res;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) {
  ZoneScoped;
  assert(m_ex);
  LogFunctionCall();

  if (pMode == NULL || pRotation == NULL)
    return D3DERR_INVALIDCALL;

  
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetDisplayModeEx, getId());
    currentUID = c.get_uid();
    c.send_data(iSwapChain);
  }
  WAIT_FOR_SERVER_RESPONSE("GetDisplayModeEx()", D3DERR_INVALIDCALL, currentUID);

  HRESULT hresult = DeviceBridge::get_data();

  if (SUCCEEDED(hresult)) {
    uint32_t len = DeviceBridge::copy_data(*pMode);
    if (len != sizeof(D3DDISPLAYMODEEX) && len != 0) {
      Logger::err("GetDisplayModeEx() failed getting display mode due to issue with data returned from server.");
      hresult = D3DERR_INVALIDCALL;
    }

    len = DeviceBridge::copy_data(*pRotation);
    if (len != sizeof(D3DDISPLAYROTATION) && len != 0) {
      Logger::err("GetDisplayModeEx() failed getting display rotation due to issue with data returned from server.");
      hresult = D3DERR_INVALIDCALL;
    }
  }
  DeviceBridge::pop_front();
  return hresult;
}

using ShaderType = BaseDirect3DDevice9Ex_LSS::ShaderConstants::ShaderType;
using ConstantType = BaseDirect3DDevice9Ex_LSS::ShaderConstants::ConstantType;
using Vec4f = BaseDirect3DDevice9Ex_LSS::ShaderConstants::Vec4<float>;
using Vec4i = BaseDirect3DDevice9Ex_LSS::ShaderConstants::Vec4<int>;

template <ShaderType   ShaderT,
  ConstantType ConstantT,
  typename     T>
HRESULT BaseDirect3DDevice9Ex_LSS::setShaderConstants(const uint32_t startRegister,
                                                      const T* const pConstantData,
                                                      const uint32_t count) {
  const auto [commonHresult, adjCount] =
    commonGetSetConstants<ShaderT, ConstantT, T>(startRegister, pConstantData, count);
  if (!SUCCEEDED(commonHresult) || adjCount == 0) {
    return commonHresult;
  }

  auto setHelper = [&](auto& set) {
    if constexpr (ConstantT == ConstantType::Float) {
      const size_t size = adjCount * sizeof(Vec4f);
      std::memcpy(set.fConsts[startRegister].data, pConstantData, size);
      if (m_stateRecording) {
        for (int i = 0; i < adjCount; i++) {
          if (ShaderT == ShaderType::Vertex) {
            m_stateRecording->m_dirtyFlags.vertexConstants.fConsts[startRegister + i] = true;
          } else {
            m_stateRecording->m_dirtyFlags.pixelConstants.fConsts[startRegister + i] = true;
          }
        }
      }
    } else if constexpr (ConstantT == ConstantType::Int) {
      const size_t size = adjCount * sizeof(Vec4i);
      std::memcpy(set.iConsts[startRegister].data, pConstantData, size);
      if (m_stateRecording) {
        for (int i = 0; i < adjCount; i++) {
          if (ShaderT == ShaderType::Vertex) {
            m_stateRecording->m_dirtyFlags.vertexConstants.iConsts[startRegister + i] = true;
          } else {
            m_stateRecording->m_dirtyFlags.pixelConstants.iConsts[startRegister + i] = true;
          }
        }
      }
    } else {
      for (uint32_t i = 0; i < adjCount; i++) {
        const uint32_t constantIdx = startRegister + i;
        const uint32_t arrayIdx = constantIdx / 32;
        const uint32_t bitIdx = constantIdx % 32;
        const uint32_t bit = 1u << bitIdx;

        set.bConsts[arrayIdx] &= ~bit;
        if (pConstantData[i]) {
          set.bConsts[arrayIdx] |= bit;
        }
        if (m_stateRecording) {
          if (ShaderT == ShaderType::Vertex) {
            m_stateRecording->m_dirtyFlags.vertexConstants.bConsts[startRegister + i] = true;
          } else {
            m_stateRecording->m_dirtyFlags.pixelConstants.bConsts[startRegister + i] = true;
          }
        }
      }
    }
    return D3D_OK;
  };
  if (m_stateRecording) {
    return ShaderT == ShaderType::Vertex
      ? setHelper(m_stateRecording->m_captureState.vertexConstants)
      : setHelper(m_stateRecording->m_captureState.pixelConstants);
  }
  return ShaderT == ShaderType::Vertex
    ? setHelper(m_state.vertexConstants)
    : setHelper(m_state.pixelConstants);
}

template <ShaderType   ShaderT,
  ConstantType ConstantT,
  typename     T>
HRESULT BaseDirect3DDevice9Ex_LSS::getShaderConstants(const uint32_t startRegister,
                                                            T* const pConstantData,
                                                      const uint32_t count) {
  const auto [commonHresult, adjCount] =
    commonGetSetConstants<ShaderT, ConstantT, T>(startRegister, pConstantData, count);
  if (!SUCCEEDED(commonHresult) || adjCount == 0) {
    return commonHresult;
  }

  auto getHelper = [&](const auto& set) {
    if constexpr (ConstantT == ConstantType::Float) {
      const float* source = set.fConsts[startRegister].data;
      const size_t size = adjCount * sizeof(Vec4f);
      std::memcpy(pConstantData, source, size);
    } else if constexpr (ConstantT == ConstantType::Int) {
      const int* source = set.iConsts[startRegister].data;
      const size_t size = adjCount * sizeof(Vec4i);
      std::memcpy(pConstantData, source, size);
    } else {
      for (uint32_t i = 0; i < adjCount; i++) {
        const uint32_t constantIdx = startRegister + i;
        const uint32_t arrayIdx = constantIdx / 32;
        const uint32_t bitIdx = constantIdx % 32;
        const uint32_t bit = (1u << bitIdx);

        const bool constValue = set.bConsts[arrayIdx] & bit;
        pConstantData[i] = constValue ? TRUE : FALSE;
      }
    }
    return D3D_OK;
  };
  return ShaderT == ShaderType::Vertex
    ? getHelper(m_state.vertexConstants)
    : getHelper(m_state.pixelConstants);
}

template <ShaderType   ShaderT,
  ConstantType ConstantT,
  typename     T>
std::tuple<HRESULT, size_t> BaseDirect3DDevice9Ex_LSS::commonGetSetConstants(const uint32_t startRegister,
                                                                            const T* const pConstantData,
                                                                            const uint32_t count) {
  const     uint32_t regCountHardware = ShaderConstants::getHardwareRegCount<ShaderT, ConstantT>();
  constexpr uint32_t regCountSoftware = ShaderConstants::getSoftwareRegCount<ShaderT, ConstantT>();
  if (startRegister + count > regCountSoftware) {
    return { D3DERR_INVALIDCALL, count };
  }
  const auto adjCount = UINT(
    std::max<INT>(
      std::clamp<INT>(count + startRegister, 0, regCountHardware) - INT(startRegister),
      0));
  if (adjCount == 0) {
    return { D3D_OK, adjCount };
  }
  if (pConstantData == nullptr) {
    return { D3DERR_INVALIDCALL, adjCount };
  }
  return { D3D_OK, adjCount };
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ResetState() {
  for (uint32_t stageIdx = 0; stageIdx < kNumStageSamplers; ++stageIdx) {
    // Reset Texture States
    m_state.textureStageStates[stageIdx][TextureStageStateType::ColorOp] = stageIdx == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    m_state.textureStageStates[stageIdx][TextureStageStateType::ColorArg1] = D3DTA_TEXTURE;
    m_state.textureStageStates[stageIdx][TextureStageStateType::ColorArg2] = D3DTA_CURRENT;
    m_state.textureStageStates[stageIdx][TextureStageStateType::AlphaOp] = stageIdx == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    // We can't predict the textures setup when do reset (many cases the textures will just be released), so keep D3DTA_TEXTURE as default state
    m_state.textureStageStates[stageIdx][TextureStageStateType::AlphaArg1] = D3DTA_TEXTURE;
    m_state.textureStageStates[stageIdx][TextureStageStateType::AlphaArg2] = D3DTA_CURRENT;
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvMat00] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvMat01] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvMat10] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvMat11] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::TexCoordIdx] = stageIdx;
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvLScale] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvLOffset] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::TexXformFlags] = D3DTTFF_DISABLE;
    m_state.textureStageStates[stageIdx][TextureStageStateType::ColorArg0] = D3DTA_CURRENT;
    m_state.textureStageStates[stageIdx][TextureStageStateType::AlphaArg0] = D3DTA_CURRENT;
    m_state.textureStageStates[stageIdx][TextureStageStateType::ResultArg] = D3DTA_CURRENT;
    m_state.textureStageStates[stageIdx][TextureStageStateType::Constant] = 0x00000000;

    // Reset Sampler States
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_ADDRESSU - 1] = D3DTADDRESS_WRAP;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_ADDRESSV - 1] = D3DTADDRESS_WRAP;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_ADDRESSW - 1] = D3DTADDRESS_WRAP;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_BORDERCOLOR - 1] = 0x00000000;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MAGFILTER - 1] = D3DTEXF_POINT;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MINFILTER - 1] = D3DTEXF_POINT;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MIPFILTER - 1] = D3DTEXF_NONE;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MIPMAPLODBIAS - 1] = 0;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MAXMIPLEVEL - 1] = 0;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MAXANISOTROPY - 1] = 1;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_SRGBTEXTURE - 1] = 0;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_ELEMENTINDEX - 1] = 0;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_DMAPOFFSET - 1] = 0;
  }

  // Referencing defaults from: https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3drenderstatetype
  m_state.renderStates[D3DRS_ZENABLE] = m_pSwapchain && m_pSwapchain->getPresentationParameters().EnableAutoDepthStencil;
  m_state.renderStates[D3DRS_FILLMODE] = D3DFILL_SOLID;
  m_state.renderStates[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
  m_state.renderStates[D3DRS_ZWRITEENABLE] = TRUE;
  m_state.renderStates[D3DRS_ALPHATESTENABLE] = FALSE;
  m_state.renderStates[D3DRS_LASTPIXEL] = TRUE;
  m_state.renderStates[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  m_state.renderStates[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  m_state.renderStates[D3DRS_CULLMODE] = D3DCULL_CCW;
  m_state.renderStates[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  m_state.renderStates[D3DRS_ALPHAREF] = 0;
  m_state.renderStates[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  m_state.renderStates[D3DRS_DITHERENABLE] = FALSE;
  m_state.renderStates[D3DRS_ALPHABLENDENABLE] = FALSE;
  m_state.renderStates[D3DRS_FOGENABLE] = FALSE;
  m_state.renderStates[D3DRS_SPECULARENABLE] = FALSE;
  m_state.renderStates[D3DRS_FOGCOLOR] = 0;
  m_state.renderStates[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
  m_state.renderStates[D3DRS_FOGSTART] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_FOGEND] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_FOGDENSITY] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_RANGEFOGENABLE] = FALSE;
  m_state.renderStates[D3DRS_STENCILENABLE] = FALSE;
  m_state.renderStates[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  m_state.renderStates[D3DRS_STENCILREF] = 0;
  m_state.renderStates[D3DRS_STENCILMASK] = 0xFFFFffff;
  m_state.renderStates[D3DRS_STENCILWRITEMASK] = 0xFFFFffff;
  m_state.renderStates[D3DRS_TEXTUREFACTOR] = 0xFFFFffff;
  for(uint32_t i=0 ; i<8 ; i++)
    m_state.renderStates[D3DRS_WRAP0+i] = 0;
  m_state.renderStates[D3DRS_CLIPPING] = TRUE;
  m_state.renderStates[D3DRS_LIGHTING] = TRUE;
  m_state.renderStates[D3DRS_AMBIENT] = 0;
  m_state.renderStates[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
  m_state.renderStates[D3DRS_COLORVERTEX] = TRUE;
  m_state.renderStates[D3DRS_LOCALVIEWER] = TRUE;
  m_state.renderStates[D3DRS_NORMALIZENORMALS] = FALSE;
  m_state.renderStates[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  m_state.renderStates[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  m_state.renderStates[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
  m_state.renderStates[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  m_state.renderStates[D3DRS_VERTEXBLEND] = D3DVBF_DISABLE;
  m_state.renderStates[D3DRS_CLIPPLANEENABLE] = 0;
  m_state.renderStates[D3DRS_POINTSIZE] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_POINTSIZE_MIN] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_POINTSPRITEENABLE] = FALSE;
  m_state.renderStates[D3DRS_POINTSCALEENABLE] = FALSE;
  m_state.renderStates[D3DRS_POINTSCALE_A] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_POINTSCALE_B] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_POINTSCALE_C] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
  m_state.renderStates[D3DRS_MULTISAMPLEMASK] = 0xFFFFffff;
  m_state.renderStates[D3DRS_PATCHEDGESTYLE] = D3DPATCHEDGE_DISCRETE;
  m_state.renderStates[D3DRS_DEBUGMONITORTOKEN] = D3DDMT_ENABLE;
  m_state.renderStates[D3DRS_POINTSIZE_MAX] = bit_cast<DWORD>(8192.f);
  m_state.renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE] = FALSE;
  m_state.renderStates[D3DRS_COLORWRITEENABLE] = 0x0000000F;
  m_state.renderStates[D3DRS_TWEENFACTOR] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
  m_state.renderStates[D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC;
  m_state.renderStates[D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR;
  m_state.renderStates[D3DRS_SCISSORTESTENABLE] = FALSE;
  m_state.renderStates[D3DRS_SLOPESCALEDEPTHBIAS] = 0;
  m_state.renderStates[D3DRS_ANTIALIASEDLINEENABLE] = FALSE;
  m_state.renderStates[D3DRS_MINTESSELLATIONLEVEL] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_MAXTESSELLATIONLEVEL] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_ADAPTIVETESS_X] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_ADAPTIVETESS_Y] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_ADAPTIVETESS_Z] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_ADAPTIVETESS_W] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_ENABLEADAPTIVETESSELLATION] = FALSE;
  m_state.renderStates[D3DRS_TWOSIDEDSTENCILMODE] = FALSE;
  m_state.renderStates[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
  m_state.renderStates[D3DRS_COLORWRITEENABLE1] = 0x0000000f;
  m_state.renderStates[D3DRS_COLORWRITEENABLE2] = 0x0000000f;
  m_state.renderStates[D3DRS_COLORWRITEENABLE3] = 0x0000000f;
  m_state.renderStates[D3DRS_BLENDFACTOR] = 0xFFFFffff;
  m_state.renderStates[D3DRS_SRGBWRITEENABLE] = 0;
  m_state.renderStates[D3DRS_DEPTHBIAS] = bit_cast<DWORD>(0.f);
  for (uint32_t i = 0; i < 8; i++)
    m_state.renderStates[D3DRS_WRAP8 + i] = 0;
  m_state.renderStates[D3DRS_SEPARATEALPHABLENDENABLE] = FALSE;
  m_state.renderStates[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  m_state.renderStates[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  m_state.renderStates[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;


  // Reset Light States
  for (uint32_t i = 0; i < caps::MaxEnabledLights; ++i) {
    m_state.bLightEnables[i] = 0;
  }

  // Reset Stream Frequency
  for (uint32_t i = 0; i < caps::MaxStreams; ++i) {
    m_state.streamFreqs[i] = 1;
  }

  // Set The Current Texture Palette entry to it's default
  // found through experimentation.
  m_curTexPalette = 65535;

  return S_OK;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::initImplicitObjects(const D3DPRESENT_PARAMETERS& presParam) {
  initImplicitSwapchain(presParam);
  initImplicitRenderTarget();
  if (presParam.EnableAutoDepthStencil) {
    initImplicitDepthStencil();
  }
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::initImplicitSwapchain(const D3DPRESENT_PARAMETERS& presParam) {
  auto* const pLssSwapChain = trackWrapper(new Direct3DSwapChain9_LSS(this, presParam));
  // To have a more consistent display when toggling windowed mode
  if (presParam.Windowed != m_previousPresentParams.Windowed && !(m_createParams.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES)) {
    SetGammaRamp(0, 0, &m_gammaRamp);
  }
  m_pSwapchain = pLssSwapChain;
  m_pSwapchain->reset(presParam);
  {
    gSwapChainMapMutex.lock();
    gSwapChainMap[m_pSwapchain->getPresentationParameters().hDeviceWindow] = { m_pSwapchain->getPresentationParameters(), this->getCreateParams(),  m_pSwapchain->getId()};
    gSwapChainMapMutex.unlock();
  }

  m_implicitRefCnt++;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::initImplicitRenderTarget() {
  IDirect3DSurface9* pRenderTarget = nullptr;

  // Creating a place-holder surface
  D3DSURFACE_DESC desc;
  desc.Width = GET_PRES_PARAM().BackBufferWidth;
  desc.Height = GET_PRES_PARAM().BackBufferHeight;
  desc.Format = GET_PRES_PARAM().BackBufferFormat;
  desc.MultiSampleType = GET_PRES_PARAM().MultiSampleType;
  desc.MultiSampleQuality = GET_PRES_PARAM().MultiSampleQuality;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.Type = D3DRTYPE_SURFACE;

  // Insert our own IDirect3DSurface9 interface implementation
  Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
  pRenderTarget = (IDirect3DSurface9*) pLssSurface;

  m_pImplicitRenderTarget = bridge_cast<Direct3DSurface9_LSS*>(pRenderTarget);
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_LinkBackBuffer, getId());
    c.send_many(0, m_pImplicitRenderTarget->getId());
  }
  m_state.renderTargets[0] = MakeD3DAutoPtr(m_pImplicitRenderTarget);
  m_implicitRefCnt++;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::initImplicitDepthStencil() {
  assert(GET_PRES_PARAM().EnableAutoDepthStencil);
  IDirect3DSurface9* pShadowDepthBuffer = nullptr;

  // Creating a place-holder surface
  D3DSURFACE_DESC desc;
  desc.Width = GET_PRES_PARAM().BackBufferWidth;
  desc.Height = GET_PRES_PARAM().BackBufferHeight;
  desc.Format = GET_PRES_PARAM().AutoDepthStencilFormat;
  desc.MultiSampleType = GET_PRES_PARAM().MultiSampleType;
  desc.MultiSampleQuality = GET_PRES_PARAM().MultiSampleQuality;
  desc.Usage = D3DUSAGE_DEPTHSTENCIL;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.Type = D3DRTYPE_SURFACE;

  Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
  pShadowDepthBuffer = (IDirect3DSurface9*) pLssSurface;

  m_pImplicitDepthStencil = bridge_cast<Direct3DSurface9_LSS*>(pShadowDepthBuffer);
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_LinkAutoDepthStencil, getId());
    c.send_data(m_pImplicitDepthStencil->getId());
  }
  m_state.depthStencil = MakeD3DAutoPtr(m_pImplicitDepthStencil);
  m_implicitRefCnt++;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::destroyImplicitObjects() {
  // Release implicit RenderTarget
  const auto rtRefCnt = m_pImplicitRenderTarget->Release();
  assert(rtRefCnt == 0 && "Implicit RenderTarget has not been released!");
  m_pImplicitRenderTarget = nullptr;
  --m_implicitRefCnt;
  m_state.renderTargets[0].reset(nullptr);

  // Release implicit DepthStencil
  if (GET_PRES_PARAM().EnableAutoDepthStencil) {
    const auto dsRefCnt = m_pImplicitDepthStencil->Release();
    assert(dsRefCnt == 0 && "Implicit DepthStencil has not been released!");
    m_pImplicitDepthStencil = nullptr;
    --m_implicitRefCnt;
    m_state.depthStencil.reset(nullptr);
  }

  const size_t nBackBuf = GET_PRES_PARAM().BackBufferCount;
  for (size_t iBackBuf = 0; iBackBuf < nBackBuf; ++iBackBuf) {
    m_pSwapchain->Release();
  }
  // Release implicit SwapChain, must happen last so PresParam still exist prior
  const auto scRefCnt = m_pSwapchain->Release();
  assert(scRefCnt == 0 && "Implicit Swapchain has not been released!");
  m_pSwapchain = nullptr;
  --m_implicitRefCnt;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::setupFPU() {
  // Should match d3d9 float behaviour.

  // For MSVC we can use these cross arch and platform funcs to set the FPU.
  // This will work on any platform, x86, x64, ARM, etc.

  // Clear exceptions.
  _clearfp();

  // Disable exceptions
  _controlfp(_MCW_EM, _MCW_EM);

  // Round to nearest
  _controlfp(_RC_NEAR, _MCW_RC);
}

// Always instantiate non-syncable variant
template class Direct3DDevice9Ex_LSS<false>;

#ifdef WITH_MULTITHREADED_DEVICE
// Do not waste code and instantiate syncable variant only when necessary
template class Direct3DDevice9Ex_LSS<true>;
#endif
