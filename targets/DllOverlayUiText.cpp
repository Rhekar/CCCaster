#include "DllOverlayUi.hpp"
#include "DllOverlayPrimitives.hpp"
#include "DllHacks.hpp"
#include "DllTrialManager.hpp"
#include "ProcessManager.hpp"
#include "Enum.hpp"
#include "Constants.hpp"

#include <windows.h>
#include <d3dx9.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <iterator>

using namespace std;
using namespace DllOverlayUi;


#define OVERLAY_FONT                    "Tahoma"

#define SCALE_FACTOR ((float)(*INITIAL_SCREEN_HEIGHT) / 640.0)

#define OVERLAY_FONT_HEIGHT             ((int)( 14.0 * SCALE_FACTOR))

#define OVERLAY_FONT_WIDTH              ((int)( 5.0 * SCALE_FACTOR))

#define OVERLAY_FONT_WEIGHT             ( 600 )

#define OVERLAY_TEXT_COLOR              D3DCOLOR_XRGB ( 255, 255, 255 )

#define OVERLAY_DEBUG_COLOR             D3DCOLOR_XRGB ( 255, 0, 0 )

#define OVERLAY_BUTTON_COLOR            D3DCOLOR_XRGB ( 0, 255, 0 )

#define OVERLAY_BUTTON_DONE_COLOR       D3DCOLOR_XRGB ( 0, 0, 255 )

#define OVERLAY_COMBO_BG_COLOR          D3DCOLOR_XRGB ( 68, 68, 68 )

#define OVERLAY_TEXT_BORDER             ( 10 )

#define OVERLAY_SELECTOR_L_COLOR        D3DCOLOR_XRGB ( 210, 0, 0 )

#define OVERLAY_SELECTOR_R_COLOR        D3DCOLOR_XRGB ( 30, 30, 255 )

#define OVERLAY_SELECTOR_X_BORDER       ( 5 )

#define OVERLAY_SELECTOR_Y_BORDER       ( 1 )

#define OVERLAY_BG_COLOR                D3DCOLOR_ARGB ( 220, 0, 0, 0 )

#define OVERLAY_CHANGE_DELTA            ( 4 + abs ( height - newHeight ) / 4 )

#define INLINE_RECT(rect)               rect.left, rect.top, rect.right, rect.bottom

#define CHAT_BOX_COLOR      			D3DCOLOR_XRGB ( 66, 229, 244 )

ENUM ( State, Disabled, Disabling, Enabled, Enabling );

static State state = State::Disabled;

ENUM ( Mode, None, Trial, Mapping );

static Mode mode = Mode::None;

static int height = 0, oldHeight = 0, newHeight = 0;

static int initialTimeout = 0, messageTimeout = 0;

static array<string, 3> text;

static array<RECT, 2> selector;

static array<bool, 2> shouldDrawSelector { false, false };

static ID3DXFont *font = 0;

static IDirect3DVertexBuffer9 *background = 0;

static array<string, 2> selectorLine;

static IDirect3DDevice9* _device = NULL; 
static D3DVIEWPORT9 _viewport;

struct Vertex
{
    FLOAT x, y, z;
    DWORD color;

    static const DWORD Format = ( D3DFVF_XYZ | D3DFVF_DIFFUSE );
};

namespace DllOverlayUi
{

array<string, 3> getText()
{
    return text;
}

array<string, 2> getSelectorLine()
{
    return selectorLine;
}

int getHeight()
{
    return height;
}

int getNewHeight()
{
    return newHeight;
}

array<RECT, 2> getSelector()
{
    return selector;
}

array<bool, 2> getShouldDrawSelector()
{
    return shouldDrawSelector;
}

void enable()
{
    TrialManager::showCombo = false;
    if ( state != State::Enabled )
        state = State::Enabling;
}

void disable()
{
    TrialManager::showCombo = true;
    if ( state != State::Disabled )
        state = State::Disabling;
}

void toggle()
{
    if ( isEnabled() )
        disable();
    else
        enable();
}

static inline int getTextHeight ( const array<string, 3>& newText )
{
    int height = 0;

    for ( const string& text : newText )
        height = max ( height, OVERLAY_FONT_HEIGHT * ( 1 + count ( text.begin(), text.end(), '\n' ) ) );

    return height;
}

void updateText()
{
    updateText ( text );
}

void updateText ( const array<string, 3>& newText )
{
    switch ( state.value )
    {
        default:
        case State::Disabled:
            height = oldHeight = newHeight = 0;
            text = { "", "", "" };
            return;

        case State::Disabling:
            newHeight = 0;

            if ( height != newHeight )
                break;

            state = State::Disabled;
            oldHeight = 0;
            text = { "", "", "" };
            break;

        case State::Enabled:
            newHeight = getTextHeight ( newText );

            if ( newHeight > height )
                break;

            if ( newHeight == height )
                oldHeight = height;

            text = newText;
            break;

        case State::Enabling:
            newHeight = getTextHeight ( newText );

            if ( height != newHeight )
                break;

            state = State::Enabled;
            oldHeight = height;
            text = newText;
            break;
    }

    if ( height == newHeight )
        return;

    if ( newHeight > height )
        height = clamped ( height + OVERLAY_CHANGE_DELTA, height, newHeight );
    else
        height = clamped ( height - OVERLAY_CHANGE_DELTA, newHeight, height );
}

bool isEnabled()
{
    return ( state != State::Disabled ) && ( messageTimeout <= 0 );
}

bool isDisabled()
{
    return ( state == State::Disabled );
}

bool isToggling()
{
    return ( state == State::Enabling || state == State::Disabling );
}

bool isTrial()
{
    return mode == Mode::Trial;
}

bool isMapping()
{
    return mode == Mode::Mapping;
}

void setNoneMode() {
	mode = Mode::None;
}

void setTrial()
{
    mode = Mode::Trial;
}

void setMapping()
{
    mode = Mode::Mapping;
}

void showMessage ( const string& newText, int timeout )
{
    // Get timeout in frames
    initialTimeout = messageTimeout = ( timeout / 17 );

    // Show the message in the middle
    text = { "", newText, "" };
    shouldDrawSelector = { false, false };

    enable();
}

void updateMessage()
{
    updateText ( text );

    if ( messageTimeout == 1 )
    {
        if ( state == State::Disabled )
            messageTimeout = 0;
        return;
    }

    if ( messageTimeout <= 2 )
    {
        disable();
        messageTimeout = 1;
        return;
    }

    // Reset message timeout when backgrounded
    if ( DllHacks::windowHandle != GetForegroundWindow() )
        messageTimeout = initialTimeout;
    else
        --messageTimeout;
}

void updateSelector ( uint8_t index, int position, const string& line )
{
    if ( index > 1 )
        return;

    selectorLine[index] = line;
    if ( position == 0 || line.empty() )
    {
        shouldDrawSelector[index] = false;
        return;
    }

    RECT rect;
    rect.top = rect.left = 0;
    rect.right = 1;
    rect.bottom = OVERLAY_FONT_HEIGHT;
    DrawText ( font, line, rect, DT_CALCRECT, D3DCOLOR_XRGB ( 0, 0, 0 ) );

    rect.top    += OVERLAY_TEXT_BORDER + position * OVERLAY_FONT_HEIGHT - OVERLAY_SELECTOR_Y_BORDER + 1;
    rect.bottom += OVERLAY_TEXT_BORDER + position * OVERLAY_FONT_HEIGHT + OVERLAY_SELECTOR_Y_BORDER;

    if ( index == 0 )
    {
        rect.left  += OVERLAY_TEXT_BORDER - OVERLAY_SELECTOR_X_BORDER;
        rect.right += OVERLAY_TEXT_BORDER + OVERLAY_SELECTOR_X_BORDER;
    }
    else
    {
        rect.left  = ( * CC_SCREEN_WIDTH_ADDR ) - rect.right - OVERLAY_TEXT_BORDER - OVERLAY_SELECTOR_X_BORDER;
        rect.right = ( * CC_SCREEN_WIDTH_ADDR ) - OVERLAY_TEXT_BORDER + OVERLAY_SELECTOR_X_BORDER;
    }

    selector[index] = rect;
    shouldDrawSelector[index] = true;
}

bool isShowingMessage()
{
    return ( messageTimeout > 0 );
}

void showChatMessage(int index, const ChatMessage& m) {

	long startHeight = (long)(_viewport.Height) - (index * OVERLAY_FONT_HEIGHT); 

	if(startHeight < 0) {
		return;
	}

	RECT r {0, startHeight, (long)_viewport.Width, startHeight + OVERLAY_FONT_HEIGHT};

	std::string msg;
	if(m.player == 0) {
		msg = m.msg;
	} else {
		msg = "P" + std::to_string(m.player) + ": " + m.msg;
	}
    DrawText ( font, msg, r, DT_WORDBREAK | DT_LEFT, OVERLAY_TEXT_COLOR );
}

void showTypingDialogue(const std::string& m) {
	
	if(_device == NULL) { // praying i dont have to deal with device loss here and its done elsewhere in the code
		return;
	}

	//DrawRectangle ( _device, INLINE_RECT(r), OVERLAY_COMBO_BG_COLOR );
	// i would love to draw something transparent, but in the interest of not copying over my entire etm draw code, i dont care.
	
	RECT r {0, ((long)_viewport.Height) - ((long)OVERLAY_FONT_HEIGHT), (long)_viewport.Width, (long)_viewport.Height};

	std::string msg = "Typing: " + m;

	DrawBox(_device, INLINE_RECT(r), 1, CHAT_BOX_COLOR);

	DrawText ( font, msg, r, DT_WORDBREAK | DT_LEFT, OVERLAY_TEXT_COLOR );
}


#ifndef RELEASE

string debugText;

int debugTextAlign = 0;

#endif // NOT RELEASE

} // namespace DllOverlayUi


void initOverlayText ( IDirect3DDevice9 *device )
{
    D3DXCreateFont ( device,                                    // device pointer
                     OVERLAY_FONT_HEIGHT,                       // height
                     OVERLAY_FONT_WIDTH,                        // width
                     OVERLAY_FONT_WEIGHT,                       // weight
                     1,                                         // # of mipmap levels
                     FALSE,                                     // italic
                     ANSI_CHARSET,                              // charset
                     OUT_DEFAULT_PRECIS,                        // output precision
                     ANTIALIASED_QUALITY,                       // quality
                     DEFAULT_PITCH | FF_DONTCARE,               // pitch and family
                     OVERLAY_FONT,                              // typeface name
                     &font );                                   // pointer to ID3DXFont

    static const Vertex verts[4] =
    {
        { -1, -1, 0, OVERLAY_BG_COLOR },
        {  1, -1, 0, OVERLAY_BG_COLOR },
        { -1,  1, 0, OVERLAY_BG_COLOR },
        {  1,  1, 0, OVERLAY_BG_COLOR },
    };

    device->CreateVertexBuffer ( 4 * sizeof ( Vertex ),         // buffer size in bytes
                                 0,                             // memory usage flags
                                 Vertex::Format,                // vertex format
                                 D3DPOOL_MANAGED,               // memory storage flags
                                 &background,                    // pointer to IDirect3DVertexBuffer9
                                 0 );                           // unused

    void *ptr;

    background->Lock ( 0, 0, ( void ** ) &ptr, 0 );
    memcpy ( ptr, verts, 4 * sizeof ( verts[0] ) );
    background->Unlock();
}

void invalidateOverlayText()
{
    if ( font )
    {
        font->OnLostDevice();
        font = 0;
    }

    if ( background )
    {
        background->Release();
        background = 0;
    }
}

void __attribute__ ((noinline)) renderOverlayText ( IDirect3DDevice9 *device, const D3DVIEWPORT9& viewport )
{
	_device = device;
	_viewport = viewport;

#ifndef RELEASE

    if ( ! debugText.empty() )
    {
        RECT rect;
        rect.top = rect.left = 0;
        rect.right = viewport.Width;
        rect.bottom = viewport.Height;

        DrawText ( font, debugText, rect, DT_WORDBREAK |
                   ( debugTextAlign == 0 ? DT_CENTER : ( debugTextAlign < 0 ? DT_LEFT : DT_RIGHT ) ),
                   OVERLAY_DEBUG_COLOR );
    }

#endif // RELEASE

    if ( ! TrialManager::dtext.empty() && !TrialManager::hideText ) {
        int debugTextAlign = 1;
        RECT rect2;
        rect2.top = rect2.left = 0;
        rect2.right = viewport.Width;
        rect2.bottom = viewport.Height;
        DrawText ( font, TrialManager::dtext, rect2, DT_WORDBREAK |
                   ( debugTextAlign == 0 ? DT_CENTER : ( debugTextAlign < 0 ? DT_LEFT : DT_RIGHT ) ),
                   OVERLAY_DEBUG_COLOR );
    }
    if ( ! TrialManager::comboTrialText.empty() && !TrialManager::hideText )
    {
        /*
        if ( TrialManager::trialTextures == NULL ) {
            char* filename = "arrow.png";
            char* filename2 = "tutorial00.bmp";
            ifstream input( filename, ios::binary );
            vector<char> buffer( istreambuf_iterator<char>(input), {} );
            int imgsize = buffer.size();
            char* rawimg = &buffer[0];
            int (*loadTextureFromMemory) (int, char*, int, int) = (int(*)(int, char*, int, int)) 0x4bd2d0;
            D3DXCreateTextureFromFile( device, filename, &TrialManager::trialTextures );
            TrialManager::trialTextures2 = loadTextureFromMemory(0, rawimg, imgsize, 0);
        }
        */
        RECT rect;
        rect.top = 70;
        rect.left = 20;
        rect.right = viewport.Width - 20;
        rect.bottom = viewport.Height - 20;
        wstring w = TrialManager::fullStrings[TrialManager::currentTrialIndex];
        TextCalcRectW( font, w, rect, DT_CENTER | DT_WORDBREAK, 0);
        rect.left = 20;
        rect.right = viewport.Width - 20;
        DrawRectangle ( device, INLINE_RECT ( rect ), OVERLAY_COMBO_BG_COLOR );
        uint32_t i = 0;
        rect.left = 30;
        for ( wstring text : TrialManager::comboTrialText ) {
            TextCalcRectW( font, text, rect, DT_LEFT, 0);
            D3DCOLOR color = ( TrialManager::comboTrialPosition > i ) ? OVERLAY_BUTTON_DONE_COLOR :
              ( TrialManager::comboTrialPosition == i ) ? OVERLAY_DEBUG_COLOR : OVERLAY_BUTTON_COLOR;
            DrawTextW ( font, text, rect, DT_WORDBREAK |
                   ( TrialManager::comboTrialTextAlign == 0 ? DT_CENTER : ( TrialManager::comboTrialTextAlign < 0 ? DT_LEFT : DT_RIGHT ) ),
                       color );
            rect.left = rect.right;
            long int newlineBreakpoint = viewport.Width - 70;
            if ( rect.left > newlineBreakpoint ) {
                rect.left = 29;
                rect.top = rect.bottom;
            }
        ++i;
        }

        RECT rect3;
        rect3.top = 50;
        rect3.left = 30;
        rect3.right = viewport.Width;
        rect3.bottom= viewport.Height;

        TextCalcRectW( font, TrialManager::comboName, rect3, DT_LEFT | DT_WORDBREAK, 0);
        rect3.left = 20;
        rect3.right = viewport.Width - 20;
        DrawRectangle ( device, INLINE_RECT ( rect3 ), OVERLAY_COMBO_BG_COLOR );
        DrawTextW ( font, TrialManager::comboName, rect3, DT_WORDBREAK | DT_LEFT,
                    OVERLAY_DEBUG_COLOR );
    }
    if ( state == State::Disabled )
        return;

    // Calculate message width if showing one
    float messageWidth = 0.0f;
    if ( isShowingMessage() )
    {
        RECT rect;
        rect.top = rect.left = 0;
        rect.right = 1;
        rect.bottom = OVERLAY_FONT_HEIGHT;

        DrawText ( font, text[1], rect, DT_CALCRECT, D3DCOLOR_XRGB ( 0, 0, 0 ) );

        messageWidth = rect.right + 2 * OVERLAY_TEXT_BORDER;
    }

    // Scaling factor for the overlay background
    const float scaleX = ( isShowingMessage() ? messageWidth / viewport.Width : 1.0f );
    const float scaleY = float ( height + 2 * OVERLAY_TEXT_BORDER ) / viewport.Height;

    D3DXMATRIX translate, scale;
    D3DXMatrixScaling ( &scale, scaleX, scaleY, 1.0f );
    D3DXMatrixTranslation ( &translate, 0.0f, 1.0f - scaleY, 0.0f );

    device->SetTexture ( 0, 0 );
    device->SetTransform ( D3DTS_VIEW, & ( scale = scale * translate ) ); // who wrote this syntax. what the hell
    device->SetStreamSource ( 0, background, 0, sizeof ( Vertex ) );
    device->SetFVF ( Vertex::Format );
    device->DrawPrimitive ( D3DPT_TRIANGLESTRIP, 0, 2 );

    // Only draw text if fully enabled or showing a message
    if ( state != State::Enabled )
        return;

    if ( ! ( text[0].empty() && text[1].empty() && text[2].empty() ) )
    {
        const int centerX = viewport.Width / 2;

        RECT rect;
        rect.left   = centerX - int ( ( viewport.Width / 2 ) * 1.0 ) + OVERLAY_TEXT_BORDER;
        rect.right  = centerX + int ( ( viewport.Width / 2 ) * 1.0 ) - OVERLAY_TEXT_BORDER;
        rect.top    = OVERLAY_TEXT_BORDER;
        rect.bottom = rect.top + height + OVERLAY_TEXT_BORDER;

        if ( newHeight == height )
        {
            if ( shouldDrawSelector[0] )
                DrawRectangle ( device, INLINE_RECT ( selector[0] ), OVERLAY_SELECTOR_L_COLOR );

            if ( shouldDrawSelector[1] )
                DrawRectangle ( device, INLINE_RECT ( selector[1] ), OVERLAY_SELECTOR_R_COLOR );
        }

        if ( ! text[0].empty() )
            DrawText ( font, text[0], rect, DT_WORDBREAK | DT_LEFT, OVERLAY_TEXT_COLOR );

        if ( ! text[1].empty() )
            DrawText ( font, text[1], rect, DT_WORDBREAK | DT_CENTER, OVERLAY_TEXT_COLOR );

        if ( ! text[2].empty() )
            DrawText ( font, text[2], rect, DT_WORDBREAK | DT_RIGHT, OVERLAY_TEXT_COLOR );
    }
}
