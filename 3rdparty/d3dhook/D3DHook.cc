#include "D3DHook.h"

#include <windows.h>
#include <d3d9.h>
#include <cassert>

#include "../minhook/include/MinHook.h"

#pragma comment(lib, "libMinHook.x86.lib")

enum d3d_offsets {
    D3D9_ADDREF = 0,
    D3D9_RELEASE = 0,
    D3D9_RESET = 16,
    D3D9_PRESENT = 17,
    D3D9_ENDSCENE = 42
};

typedef LPDIRECT3D9 (WINAPI* d3d9CreateFn)(UINT);
typedef HRESULT ( __stdcall *ResetFn ) ( IDirect3DDevice9 *pDevice, LPVOID );
typedef HRESULT ( __stdcall *PresentFn ) ( IDirect3DDevice9 *pDevice, const RECT *, const RECT *, HWND, LPVOID );
typedef HRESULT ( __stdcall *EndSceneFn ) ( IDirect3DDevice9 *pDevice);

ResetFn old_reset;
PresentFn old_present;
EndSceneFn old_endscene;

static void **VTable;

HRESULT __stdcall hook_reset(IDirect3DDevice9 *device, LPVOID params) {
    InvalidateDeviceObjects();
    HRESULT res = old_reset(device, params);
    return res;
}

HRESULT __stdcall hook_present(IDirect3DDevice9 *device, const RECT *src, const RECT *dest, HWND window, LPVOID unused) {
    PresentFrameBegin(device);
    HRESULT res = old_present(device, src, dest, window, unused);
    PresentFrameEnd(device);
    return res;
}

HRESULT __stdcall hook_endscene(IDirect3DDevice9 *device) {
    EndScene(device);
    HRESULT res = old_endscene(device);
    return res;
}

std::string InitDirectX(void *hwnd) {
    // Get VTable From Dummy Device
    HMODULE d3d9_module = GetModuleHandleA("d3d9.dll");
    if (d3d9_module == NULL) {
        return "failed to get the d3d9.dll module handle";
    }

    d3d9CreateFn create_fn = (d3d9CreateFn)GetProcAddress(d3d9_module, "Direct3DCreate9");

    IDirect3D9 *direct3d = create_fn(D3D_SDK_VERSION);

    D3DPRESENT_PARAMETERS d3dparams = {0};
    d3dparams.Windowed = true;
    d3dparams.SwapEffect = D3DSWAPEFFECT_DISCARD;

    IDirect3DDevice9 *device;
    direct3d->CreateDevice(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        (HWND)hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &d3dparams,
        &device
    );

    VTable = *(void ***)device;

    // Cleanup
    device->Release();
    direct3d->Release();

    return "";
}

std::string HookDirectX() {
    if (MH_CreateHook(VTable[D3D9_RESET], (void*)&hook_reset, reinterpret_cast<LPVOID*>(&old_reset)) != MH_OK) {
        return "failed to hook reset";
    }
    if (MH_CreateHook(VTable[D3D9_PRESENT], (void*)&hook_present, reinterpret_cast<LPVOID*>(&old_present)) != MH_OK) {
        return "failed to hook present";
    }
    if (MH_CreateHook(VTable[D3D9_ENDSCENE], (void*)&hook_endscene, reinterpret_cast<LPVOID*>(&old_endscene)) != MH_OK) {
        return "failed to hook endscene";
    }

    if (MH_EnableHook(VTable[D3D9_RESET]) != MH_OK) {
        return "failed to enable reset hook";
    }
    if (MH_EnableHook(VTable[D3D9_PRESENT]) != MH_OK) {
        return "failed to enable present hook";
    }
    if (MH_EnableHook(VTable[D3D9_ENDSCENE]) != MH_OK) {
        return "failed to enable endscene hook";
    }

    return "";
}

void UnhookDirectX() {
    if (MH_DisableHook(VTable[D3D9_RESET]) != MH_OK) {
        assert(false);
    }
    if (MH_DisableHook(VTable[D3D9_PRESENT]) != MH_OK) {
        assert(false);
    }
    if (MH_DisableHook(VTable[D3D9_ENDSCENE]) != MH_OK) {
        assert(false);
    }
}

