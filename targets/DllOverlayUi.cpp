#include "DllOverlayUi.hpp"
#include "ProcessManager.hpp"
#include "Constants.hpp"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <d3dx9.h>

using namespace std;
using namespace DllOverlayUi;


bool initalizedDirectX = false;

static bool shouldInitDirectX = false;

bool doEndScene = false;

namespace DllOverlayUi
{

void init()
{
    shouldInitDirectX = true;
}

} // namespace DllOverlayUi


void initOverlayText ( IDirect3DDevice9 *device );

void invalidateOverlayText();

void renderOverlayText ( IDirect3DDevice9 *device, const D3DVIEWPORT9& viewport );

void initImGui( IDirect3DDevice9 *device );

void InitializeDirectX ( IDirect3DDevice9 *device )
{
    if ( ! shouldInitDirectX )
        return;

    initalizedDirectX = true;

    initOverlayText ( device );
#ifdef LOGGING
    initImGui ( device );
#endif
}

void InvalidateDeviceObjects()
{
    if ( ! initalizedDirectX )
        return;

    initalizedDirectX = false;

    invalidateOverlayText();
#ifdef LOGGING
    // Release ImGui's D3DPOOL_DEFAULT resources (vertex/index buffers, font texture, state block)
    // BEFORE the game resets the device (e.g. alt+enter fullscreen toggle). Otherwise Reset() fails
    // on the still-live default-pool resources and ImGui then renders against freed memory, which
    // faults inside hook.dll. The next ImGui_ImplDX9_NewFrame() recreates them automatically.
    ImGui_ImplDX9_InvalidateDeviceObjects();
#endif
}

// Note: this is called on the SAME thread as the main application thread
void PresentFrameBegin ( IDirect3DDevice9 *device )
{
    if ( ! initalizedDirectX )
        InitializeDirectX ( device );

    D3DVIEWPORT9 viewport;
    device->GetViewport ( &viewport );

    // Only draw in the main viewport; there should only be one with this width
    if ( viewport.Width != * CC_SCREEN_WIDTH_ADDR )
        return;

    renderOverlayText ( device, viewport );
#ifdef LOGGING
    doEndScene = true;
    if (device->BeginScene() >= 0)
    {
        // Imgui needs to be called on the EndScene right before present is called
        // because there's about 100 begin/endscene pairs between present calls
        device->EndScene();
    }
#endif
}
