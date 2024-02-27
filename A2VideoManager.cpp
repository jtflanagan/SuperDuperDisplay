#include "A2VideoManager.h"
#include <cstring>
#include <zlib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <map>
#include "SDL.h"
#ifdef _DEBUGTIMINGS
#include <chrono>
#endif

#include "OpenGLHelper.h"
#include "SDHRManager.h"
#include "CycleCounter.h"
#include "GRAddr2XY.h"

static inline uint32_t SETRGBCOLOR(uint8_t r, uint8_t g, uint8_t b)
{
	return ((0xFF << 24) | (b << 16) | (g << 8) | r);
}

static uint32_t gPaletteRGB[] =
{
	// HiRes
	SETRGBCOLOR(/*HGR_BLACK, */ 0x00,0x00,0x00),
	SETRGBCOLOR(/*HGR_WHITE, */ 0xFF,0xFF,0xFF),
	SETRGBCOLOR(/*BLUE,      */ 0x00,0x8A,0xB5),
	SETRGBCOLOR(/*ORANGE,    */ 0xFF,0x72,0x47),
	SETRGBCOLOR(/*GREEN,     */ 0x6F,0xE6,0x2C),
	SETRGBCOLOR(/*MAGENTA,   */ 0xAA,0x1A,0xD1),

	// TV emu
	SETRGBCOLOR(/*HGR_GREY1, */ 0x80,0x80,0x80),
	SETRGBCOLOR(/*HGR_GREY2, */ 0x80,0x80,0x80),
	SETRGBCOLOR(/*HGR_YELLOW,*/ 0x9E,0x9E,0x00),
	SETRGBCOLOR(/*HGR_AQUA,  */ 0x00,0xCD,0x4A),
	SETRGBCOLOR(/*HGR_PURPLE,*/ 0x61,0x61,0xFF),
	SETRGBCOLOR(/*HGR_PINK,  */ 0xFF,0x32,0xB5),

	// LoRes
	SETRGBCOLOR(/*BLACK,*/      0x00,0x00,0x00),
	SETRGBCOLOR(/*DEEP_RED,*/   0xAC,0x12,0x4C),
	SETRGBCOLOR(/*DARK_BLUE,*/  0x00,0x07,0x83),
	SETRGBCOLOR(/*MAGENTA,*/    0xAA,0x1A,0xD1),
	SETRGBCOLOR(/*DARK_GREEN,*/ 0x00,0x83,0x2F),
	SETRGBCOLOR(/*DARK_GRAY,*/  0x9F,0x97,0x7E),
	SETRGBCOLOR(/*BLUE,*/       0x00,0x8A,0xB5),
	SETRGBCOLOR(/*LIGHT_BLUE,*/ 0x9F,0x9E,0xFF),
	SETRGBCOLOR(/*BROWN,*/      0x7A,0x5F,0x00),
	SETRGBCOLOR(/*ORANGE,*/     0xFF,0x72,0x47),
	SETRGBCOLOR(/*LIGHT_GRAY,*/ 0x78,0x68,0x7F),
	SETRGBCOLOR(/*PINK,*/       0xFF,0x7A,0xCF),
	SETRGBCOLOR(/*GREEN,*/      0x6F,0xE6,0x2C),
	SETRGBCOLOR(/*YELLOW,*/     0xFF,0xF6,0x7B),
	SETRGBCOLOR(/*AQUA,*/       0x6C,0xEE,0xB2),
	SETRGBCOLOR(/*WHITE,*/      0xFF,0xFF,0xFF),
};

// Gotta have a transparency global just in case
// static uint32_t gRGBTransparent = 0;

// below because "The declaration of a static data member in its class definition is not a definition"
A2VideoManager* A2VideoManager::s_instance;
uint16_t A2VideoManager::a2SoftSwitches = 0;
uint8_t A2VideoManager::switch_c022 = 0;
uint8_t A2VideoManager::switch_c034 = 0;

constexpr uint32_t CYCLES_HBLANK = 25;			// always 25 cycles

static OpenGLHelper* oglHelper = OpenGLHelper::GetInstance();

static Shader shader_text = Shader();
static Shader shader_lgr = Shader();
static Shader shader_hgr = Shader();
static Shader shader_dhgr = Shader();
static Shader shader_shr = Shader();
static Shader shader_beam_legacy = Shader();
static Shader shader_beam_shr = Shader();

//////////////////////////////////////////////////////////////////////////
// Image Asset Methods
//////////////////////////////////////////////////////////////////////////

// NOTE:	Both the below image asset methods use OpenGL 
//			so they _must_ be called from the main thread
void A2VideoManager::ImageAsset::AssignByFilename(A2VideoManager* owner, const char* filename) {
	int width;
	int height;
	int channels;
	unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);
	if (data == NULL) {
		std::cerr << "ERROR: STBI load failure" << stbi_failure_reason() << std::endl;
		return;
	}
	if (tex_id != UINT_MAX)
	{
		oglHelper->load_texture(data, width, height, channels, tex_id);
		stbi_image_free(data);
	}
	else {
		std::cerr << "ERROR: Could not bind texture, all slots filled!" << std::endl;
		return;
	}
	GLenum glerr;
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "ImageAsset::AssignByFilename error: " << glerr << std::endl;
	}
	image_xcount = width;
	image_ycount = height;
}

//////////////////////////////////////////////////////////////////////////
// Manager Methods
//////////////////////////////////////////////////////////////////////////

void A2VideoManager::Initialize()
{
	memset(a2legacy_vram, 0, _BEAM_VRAM_SIZE_LEGACY);
	memset(a2shr_vram, 0, _BEAM_VRAM_SIZE_SHR);
	bVBlankHasLegacy = true;
	bVBlankHasSHR = false;
	
	v_fblgr = std::vector<uint32_t>(_A2VIDEO_MIN_WIDTH * _A2VIDEO_MIN_HEIGHT, 0);
	v_fbdlgr = std::vector<uint32_t>(_A2VIDEO_MIN_WIDTH * _A2VIDEO_MIN_HEIGHT * 2, 0);
	v_fbhgr = std::vector<uint32_t>(_A2VIDEO_MIN_WIDTH * _A2VIDEO_MIN_HEIGHT, 0);
	v_fbdhgr = std::vector<uint32_t>(_A2VIDEO_MIN_WIDTH * _A2VIDEO_MIN_HEIGHT * 2, 0);
	v_fbshr = std::vector<uint32_t>(_A2VIDEO_SHR_WIDTH * _A2VIDEO_SHR_HEIGHT, 0);
	
	color_border = 0;
	color_foreground = UINT32_MAX;
	color_background = 0;

	a2SoftSwitches = A2SS_TEXT; // default to TEXT1

	// Set up the image assets (textures)
	// Assign them their respective GPU texture id
	*image_assets = {};
	for (size_t i = 0; i < (sizeof(image_assets) / sizeof(ImageAsset)); i++)
	{
		image_assets[i].tex_id = oglHelper->get_texture_id_at_slot(i);
	}

	// Generate shaders
	shader_text.build(_SHADER_A2_VERTEX_DEFAULT, _SHADER_TEXT_FRAGMENT);
	shader_lgr.build(_SHADER_A2_VERTEX_DEFAULT, _SHADER_LGR_FRAGMENT);
	shader_hgr.build(_SHADER_A2_VERTEX_DEFAULT, _SHADER_HGR_FRAGMENT);
	shader_dhgr.build(_SHADER_A2_VERTEX_DEFAULT, _SHADER_DHGR_FRAGMENT);
	shader_shr.build(_SHADER_A2_VERTEX_DEFAULT, _SHADER_SHR_FRAGMENT);
	shader_beam_legacy.build(_SHADER_A2_VERTEX_DEFAULT, _SHADER_BEAM_LEGACY_FRAGMENT);
	shader_beam_shr.build(_SHADER_A2_VERTEX_DEFAULT, _SHADER_BEAM_LEGACY_FRAGMENT);		// TODO: MAKE SHR BEAM SHADER

	// Initialize windows and meshes
	
	windows[A2VIDEO_TEXT].Define(A2VIDEO_TEXT, &shader_text);
	windows[A2VIDEO_DTEXT].Define(A2VIDEO_DTEXT, &shader_text);
	windows[A2VIDEO_LGR].Define(A2VIDEO_LGR, &shader_lgr);
	windows[A2VIDEO_DLGR].Define(A2VIDEO_DLGR, &shader_lgr);
	windows[A2VIDEO_HGR].Define(A2VIDEO_HGR, &shader_hgr);
	windows[A2VIDEO_DHGR].Define(A2VIDEO_DHGR, &shader_dhgr);
	windows[A2VIDEO_SHR].Define(A2VIDEO_SHR, &shader_shr);
	
	windowsbeam[A2VIDEOBEAM_LEGACY].Define(A2VIDEOBEAM_LEGACY, &shader_beam_legacy);
	windowsbeam[A2VIDEOBEAM_SHR].Define(A2VIDEOBEAM_SHR, &shader_beam_shr);

	// Activate TEXT by default
	SelectVideoModes();
	// tell the next Render() call to run initialization routines
	bShouldInitializeRender = true;
}

A2VideoManager::~A2VideoManager()
{
	delete[] a2legacy_vram;
	delete[] a2shr_vram;
}

void A2VideoManager::ResetComputer()
{
    if (bIsRebooting == true)
        return;
    bIsRebooting = true;
    SDHRManager::GetInstance()->Initialize();
	this->Initialize();
    bIsRebooting = false;
}

void A2VideoManager::SetSoftSwitch(A2SoftSwitch_e ss, bool state)
{
	if (state)
		a2SoftSwitches |= ss;
	else
		a2SoftSwitches &= ~ss;
	A2VideoManager::GetInstance()->ToggleA2Video(bA2VideoEnabled);
}

void A2VideoManager::NotifyA2MemoryDidChange(uint16_t addr)
{
	// Note: We could do delta updates here for the video modes
	// but for better reliability we do full updates of the video modes
	// every frame in the render method
	
	// Do nothing, we assume that the active video mode should always refresh its memory
}

void A2VideoManager::ToggleA2Video(bool value)
{
	bA2VideoEnabled = value;
	if (bA2VideoEnabled)
	{
		SelectVideoModes();
		auto scrSz = ScreenSize();
		if (oglHelper->request_framebuffer_resize(scrSz.x, scrSz.y))
			bShouldInitializeRender = true;
	}
}

void A2VideoManager::ProcessSoftSwitch(uint16_t addr, uint8_t val, bool rw, bool is_iigs)
{
    //std::cerr << "Processing soft switch " << std::hex << (uint32_t)addr << " RW: " << (uint32_t)rw << " 2gs: " << (uint32_t)is_iigs << std::endl;
	switch (addr)
	{
	case 0xC000:	// 80STOREOFF
        if (!rw)
            a2SoftSwitches &= ~A2SS_80STORE;
		break;
	case 0xC001:	// 80STOREON
        if (!rw)
            a2SoftSwitches |= A2SS_80STORE;
		break;
	case 0xC002:	// RAMRDOFF
        if (!rw)
            a2SoftSwitches &= ~A2SS_RAMRD;
		break;
	case 0xC003:	// RAMRDON
        if (!rw)
            a2SoftSwitches |= A2SS_RAMRD;
		break;
	case 0xC004:	// RAMWRTOFF
        if (!rw)
            a2SoftSwitches &= ~A2SS_RAMWRT;
		break;
	case 0xC005:	// RAMWRTON
        if (!rw)
            a2SoftSwitches |= A2SS_RAMWRT;
		break;
	case 0xC006:	// INTCXROMOFF
        if (!rw)
            a2SoftSwitches &= ~A2SS_INTCXROM;
		break;
	case 0xC007:	// INTCXROMON
        if (!rw)
            a2SoftSwitches |= A2SS_INTCXROM;
		break;
	case 0xC00A:	// SLOTC3ROMOFF
        if (!rw)
            a2SoftSwitches &= ~A2SS_SLOTC3ROM;
		break;
	case 0xC00B:	// SLOTC3ROMOFF
        if (!rw)
            a2SoftSwitches |= A2SS_SLOTC3ROM;
		break;
	case 0xC00C:	// 80COLOFF
        if (!rw)
            a2SoftSwitches &= ~A2SS_80COL;
		break;
	case 0xC00D:	// 80COLON
        if (!rw)
            a2SoftSwitches |= A2SS_80COL;
		break;
	case 0xC00E:	// ALTCHARSETOFF
        if (!rw)
            a2SoftSwitches &= ~A2SS_ALTCHARSET;
		break;
	case 0xC00F:	// ALTCHARSETON
        if (!rw)
            a2SoftSwitches |= A2SS_ALTCHARSET;
		break;
	case 0xC050:	// TEXTOFF
		a2SoftSwitches &= ~A2SS_TEXT;
		break;
	case 0xC051:	// TEXTON
		a2SoftSwitches |= A2SS_TEXT;
		break;
	case 0xC052:	// MIXEDOFF
		a2SoftSwitches &= ~A2SS_MIXED;
		break;
	case 0xC053:	// MIXEDON
		a2SoftSwitches |= A2SS_MIXED;
		break;
	case 0xC054:	// PAGE2OFF
		a2SoftSwitches &= ~A2SS_PAGE2;
		break;
	case 0xC055:	// PAGE2ON
		a2SoftSwitches |= A2SS_PAGE2;
		break;
	case 0xC056:	// HIRESOFF
		a2SoftSwitches &= ~A2SS_HIRES;
		break;
	case 0xC057:	// HIRESON
		a2SoftSwitches |= A2SS_HIRES;
		break;
	case 0xC05E:	// DHIRESON
		a2SoftSwitches |= A2SS_DHGR;
		break;
	case 0xC05F:	// DHIRESOFF
		a2SoftSwitches &= ~A2SS_DHGR;
		break;
	case 0xC021:	// MONOCOLOR
		// bits 0-6 are reserved
		// bit 7 determines color or greyscale. Greyscale is 1
		if (!rw)
		{
			SetSoftSwitch(A2SS_GREYSCALE, (bool)(val >> 7));
		}
		break;
	// $C022   R / W     SCREENCOLOR[IIgs] text foreground and background colors(also VidHD)
	case 0xC022:	// Set screen color
//            std::cerr << "Processing soft switch " << std::hex << (uint32_t)addr <<
//            " VAL: " << (uint32_t)val <<
//            " RW: " << (uint32_t)rw << std::endl;
        if (!rw)
        {
			switch_c022 = val;
            color_background = gPaletteRGB[12 + (val & 0x0F)];
            color_foreground = gPaletteRGB[12 + ((val & 0xF0) >> 4)];
        }
		break;
	// $C034   R / W     BORDERCOLOR[IIgs] b3:0 are border color(also VidHD)
	case 0xC034:	// Set border color on bits 3:0
        if (!rw)
		{
			switch_c034 = val;
			color_border = gPaletteRGB[12 + (val & 0x0F)];
		}
		break;
    // $C029   R/W     NEWVIDEO        [IIgs] Select new video modes (also VidHD)
    case 0xC029:    // NEWVIDEO (SHR)
		if (rw)		// don't do anything on read
			break;
		// bits 1-4 are reserved
		if (val == 0x21)
		{
			// Return to mode TEXT
			a2SoftSwitches &= ~A2SS_SHR;
			a2SoftSwitches |= A2SS_TEXT;
			break;
		}
		if (val & 0x20)		// bit 5
		{
			// DHGR in Monochrome @ 560x192
			a2SoftSwitches |= A2SS_DHGRMONO;
		} else {
			// DHGR in 16 Colors @ 140x192
			a2SoftSwitches &= ~A2SS_DHGRMONO;
		}
		if (val & 0x40)	// bit 6
		{
			// Video buffer is contiguous 0x2000-0x9D00 in bank E1
		} else {
			// AUX memory behaves like Apple //e
		}
		if (val & 0x80)	// bit 7
		{
			// SHR video mode. Bit 6 is considered on
			a2SoftSwitches |= A2SS_SHR;
			CycleCounter::GetInstance()->isSHR = true;
		} else {
			// Classic Apple 2 video modes
			a2SoftSwitches &= ~A2SS_SHR;
			CycleCounter::GetInstance()->isSHR = false;
		}
        break;
	default:
		break;
	}
	ToggleA2Video(bA2VideoEnabled);	// force video refresh
}

void A2VideoManager::BeamIsAtPosition(uint32_t _x, uint32_t y)
{
	if (_x < CYCLES_HBLANK)	// in HBLANK, nothing to do
		return;
	// Theoretically at y==192 (start of VBLANK) we can render for legacy
	// but SHR goes to 200 so let's wait until 200 anyway. We're in VBLANK still.
	if (_x == CYCLES_HBLANK && y == 200)	// Start of VBLANK
	{
		RequestBeamRendering(bVBlankHasLegacy, bVBlankHasSHR);
		// reset the legacy and shr flags at each vblank
		bVBlankHasLegacy = false;
		bVBlankHasSHR = false;
		return;
	}
	if (y >= 200)	// in VBLANK, nothing to do
		return;
	
	// Set xx to 0 when after HBLANK. HBLANK is always at the start of the line
	// However, VBLANK is at the end of the screen so we can use y as is
	auto xx = _x - CYCLES_HBLANK;
	if (IsSoftSwitch(A2SS_SHR))
	{
		bVBlankHasSHR = true;		// at least 1 byte in this vblank cycle is in SHR
		auto lineStartPtr = a2shr_vram + (1 + 32 + 160) * y;
		auto memPtr = SDHRManager::GetInstance()->GetApple2MemAuxPtr();
		if (xx == 0)
		{
			// it's the beginning of the line
			// Get the SCB
			lineStartPtr[0] = *(memPtr + _A2VIDEO_SHR_SCB_START + y);
			// Get the palettes
			memcpy(lineStartPtr + 1,	// palette starts at byte 1 in our a2shr_vram
				   memPtr + _A2VIDEO_SHR_PALETTE_START + ((uint32_t)(lineStartPtr[0] & 0xFu) << 5),
				   32);					// palette length is 32 bytes
		}
		// Get the color info
		lineStartPtr[1 + 32 + xx] = *(memPtr + _A2VIDEO_SHR_START + y * _A2VIDEO_SHR_BYTES_PER_LINE + xx);
		return;
	}
	
	// The byte isn't SHR, it's legacy
	bVBlankHasLegacy = true;	// at least 1 byte in this vblank cycle is not SHR
	auto byteStartPtr = a2legacy_vram + (40 * 4 * y) + (xx * 4);
	// the flags byte is:
	// bits 0-2: mode (TEXT, DTEXT, LGR, DLGR, HGR, DHGR, DHGRMONO)
	// bit 3: ALT charset for TEXT
	// bits 4-7: border color (like in the 2gs)
	uint8_t flags = 0;
	// the colors byte is:
	// bits 0-3: background color
	// bits 4-7: foreground color
	uint8_t colors = 0;
	
	// now set the mode, and depending on the mode, grab the bytes
	if (!IsSoftSwitch(A2SS_TEXT))
	{
		if (IsSoftSwitch(A2SS_MIXED) && y > 159)	// check mixed mode
		{
			if (IsSoftSwitch(A2SS_80COL))
				flags = 1;	// DTEXT
			else
				flags = 0;	// TEXT
		}
		else if (IsSoftSwitch(A2SS_80COL) && IsSoftSwitch(A2SS_DHGR))	// double resolution
		{
			if (IsSoftSwitch(A2SS_HIRES))
				flags = 5;	// DHGR
			else
				flags = 3;	// DLGR
		}
		else if (IsSoftSwitch(A2SS_HIRES))	// standard hires
		{
			flags = 4;	// HGR
		}
		else {	// standard lores
			flags = 2;	// LGR
		}
	}
	// Now check the text modes
	if (IsSoftSwitch(A2SS_TEXT) || IsSoftSwitch(A2SS_MIXED))
	{
		if (IsSoftSwitch(A2SS_80COL))
			flags = 1;	// DTEXT
		else
		{
			flags = 0;	// TEXT
		}
	}
	
	// Fill in the rest of the flags. We already use bits 0-2 for the modes
	flags += ((IsSoftSwitch(A2SS_ALTCHARSET) ? 1 : 0) << 3);	// bit 3 is alt charset
	flags += ((switch_c034 & 0b111) << 4);						// bits 4-7 are border color
																// and the colors
	colors = switch_c022;
	// Check for page 2
	bool isPage2 = false;
	// Careful: it's only page 2 if 80STORE is off
	if (IsSoftSwitch(A2SS_PAGE2) && !IsSoftSwitch(A2SS_80STORE))
		isPage2 = true;
	
	// Finally set the 4 VRAM bytes
	// Determine where in memory we should get the data from, and get it
	if (flags < 4)	// D/TEXT AND D/LGR
	{
		uint32_t startMem = _A2VIDEO_TEXT1_START;
		if ((flags < 3) && isPage2)		// check for page 2 (DLGR doesn't have it)
			startMem = _A2VIDEO_TEXT2_START;
		byteStartPtr[0] = *(SDHRManager::GetInstance()->GetApple2MemPtr() + startMem + ((y / 8) * 40) + xx);
		byteStartPtr[1] = *(SDHRManager::GetInstance()->GetApple2MemAuxPtr() + startMem + ((y / 8) * 40) + xx);
	} else {		// D/HIRES
		uint32_t startMem = _A2VIDEO_HGR1_START;
		if (isPage2)
			startMem = _A2VIDEO_HGR2_START;
		byteStartPtr[0] = *(SDHRManager::GetInstance()->GetApple2MemPtr() + startMem + (y * 40) + xx);
		byteStartPtr[1] = *(SDHRManager::GetInstance()->GetApple2MemAuxPtr() + startMem + (y * 40) + xx);
	}
	byteStartPtr[2] = flags;
	byteStartPtr[3] = colors;

}

void A2VideoManager::RequestBeamRendering(bool cycleHasLegacy, bool cycleHasSHR)
{
	std::lock_guard<std::mutex> lock(a2video_mutex);
	bBeamRenderLegacy = cycleHasLegacy;
	bBeamRenderSHR = cycleHasSHR;
	bRequestBeamRendering = true;
}

void A2VideoManager::SelectVideoModes()
{
	for (auto& _w : this->windows) {
		_w.SetEnabled(false);
	}
	
	// SHR overrides all other modes
	if (IsSoftSwitch(A2SS_SHR))
	{
		this->windows[A2VIDEO_SHR].SetEnabled(true);
		return;
	}
	
	// Other Apple 2 modes, starting with graphics
	if (!IsSoftSwitch(A2SS_TEXT))
	{
		if (IsSoftSwitch(A2SS_80COL) && IsSoftSwitch(A2SS_DHGR))	// double resolution
		{
			if (IsSoftSwitch(A2SS_HIRES))
				this->windows[A2VIDEO_DHGR].SetEnabled(true);
			else
				this->windows[A2VIDEO_DLGR].SetEnabled(true);
		}
		else if (IsSoftSwitch(A2SS_HIRES))	// standard hires
		{
			this->windows[A2VIDEO_HGR].SetEnabled(true);
		}
		else {	// standard lores
			this->windows[A2VIDEO_LGR].SetEnabled(true);
		}
	}
	// Now check the text modes
	if (IsSoftSwitch(A2SS_TEXT) || IsSoftSwitch(A2SS_MIXED))
	{
		if (IsSoftSwitch(A2SS_80COL))
			this->windows[A2VIDEO_DTEXT].SetEnabled(true);
		else
		{
			this->windows[A2VIDEO_TEXT].SetEnabled(true);
		}
	}
	return;
}

uXY A2VideoManager::ScreenSize()
{
	uXY maxSize = uXY({ _A2VIDEO_MIN_WIDTH, _A2VIDEO_MIN_HEIGHT});
	uXY s = maxSize;
	for (auto& _w : this->windows) {
		if (_w.IsEnabled())
		{
			s = _w.Get_screen_count();
			maxSize.x = (s.x > maxSize.x ? s.x : maxSize.x);
			maxSize.y = (s.y > maxSize.y ? s.y : maxSize.y);
		}
	}
	return maxSize;
}

void A2VideoManager::Render()
{
	if (!bA2VideoEnabled)
		return;

	GLenum glerr;
	auto oglh = OpenGLHelper::GetInstance();

	oglh->setup_render();

	// Initialization routine runs only once on init (or re-init)
	// We do that here because we know the framebuffer is bound, and everything
	// for drawing the SDHR stuff is active
	if (bShouldInitializeRender) {
		bShouldInitializeRender = false;

		// image asset 0: The apple 2e US font
		glActiveTexture(_SDHR_START_TEXTURES);
		image_assets[0].AssignByFilename(this, "assets/Apple2eFont14x16 - Regular.png");
		// image asset 1: The alternate font
		glActiveTexture(_SDHR_START_TEXTURES + 1);
		image_assets[1].AssignByFilename(this, "assets/Apple2eFont14x16 - Alternate.png");
		// image asset 2: The apple 2e US font 80COL
		glActiveTexture(_SDHR_START_TEXTURES + 2);
		image_assets[2].AssignByFilename(this, "assets/Apple2eFont7x16 - Regular.png");
		// image asset 3: The alternate font 80COL
		glActiveTexture(_SDHR_START_TEXTURES + 3);
		image_assets[3].AssignByFilename(this, "assets/Apple2eFont7x16 - Alternate.png");
		// image asset 4: LGR texture (overkill for color, useful for dithered b/w)
		glActiveTexture(_SDHR_START_TEXTURES + 4);
		image_assets[4].AssignByFilename(this, "assets/Texture_composite_lgr.png");
		// image asset 5: HGR texture
		glActiveTexture(_SDHR_START_TEXTURES + 5);
		image_assets[5].AssignByFilename(this, "assets/Texture_composite_hgr.png");
		// image asset 6: DHGR texture
		glActiveTexture(_SDHR_START_TEXTURES + 6);
		image_assets[6].AssignByFilename(this, "assets/Texture_composite_dhgr.png");
		// image asset 7: The bezel for postprocessing
		glActiveTexture(_SDHR_START_TEXTURES + 7);
		image_assets[7].AssignByFilename(this, "assets/Bezel.png");
		if ((glerr = glGetError()) != GL_NO_ERROR) {
			std::cerr << "OpenGL AssignByFilename error: " 
				<< 0 << " - " << glerr << std::endl;
		}
		glActiveTexture(GL_TEXTURE0);
	}

	// BEAM RENDERER
	if (bShouldUseBeamRenderer)
	{
		// At line 200 the cycle counter flags to update the VRAM in the GPU
		a2video_mutex.lock();
		bool shouldUpdateDataInGPU = bRequestBeamRendering;
		windowsbeam[A2VIDEOBEAM_LEGACY].SetEnabled(bBeamRenderLegacy);
		windowsbeam[A2VIDEOBEAM_SHR].SetEnabled(bBeamRenderSHR);
		bRequestBeamRendering = false;
		a2video_mutex.unlock();
		windowsbeam[A2VIDEOBEAM_LEGACY].Render(shouldUpdateDataInGPU);
		windowsbeam[A2VIDEOBEAM_SHR].Render(shouldUpdateDataInGPU);
		goto ENDRENDER;
	}
	
	// GPU RENDERER
	for (auto& _w : this->windows) {
		if (bShouldUseCPURGBRenderer)
		{
			// Only GPU render the text modes
			auto _vidM = _w.Get_video_mode();
			if ((_vidM == A2VIDEO_TEXT) || (_vidM == A2VIDEO_DTEXT))
			{
				_w.Update();
				_w.Render();
			}
		}
		else {
			if (!this->windows[A2VIDEO_SHR].IsEnabled() || (_w.Get_video_mode() == A2VIDEO_SHR))
			{
				// When SHR is enabled, it turns off every other mode
				_w.Render();
			}
		}
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, oglHelper->get_intermediate_texture_id());

	// CPU RENDERER
	if (bShouldUseCPURGBRenderer)
	{
		// Check for page 2
		bool isPage2 = false;
		// Careful: it's only page 2 if 80STORE is off
		if (IsSoftSwitch(A2SS_PAGE2) && !IsSoftSwitch(A2SS_80STORE))
			isPage2 = true;

		if (this->windows[A2VIDEO_SHR].IsEnabled())
		{
			this->windows[A2VIDEO_SHR].Update();
			// Update one line at a time
			// We doubled the SHR pixel height so only send 1 of 2 lines
			for (uint8_t i = 0; i < _A2VIDEO_SHR_HEIGHT / 2; i++)
			{
				this->UpdateSHRLine(i, &v_fbshr);
			}
			this->RenderSubMixed(&v_fbshr);
			goto ENDRENDER;
		}
		if (this->windows[A2VIDEO_LGR].IsEnabled())
		{
			this->windows[A2VIDEO_LGR].Update();
			for (size_t i = 0; i < _A2VIDEO_TEXT_SIZE; i++)
			{
				this->UpdateLoResRGBCell(
					(isPage2 ? _A2VIDEO_TEXT2_START : _A2VIDEO_TEXT1_START) + i,
					(isPage2 ? _A2VIDEO_TEXT2_START : _A2VIDEO_TEXT1_START),
					&v_fblgr);
			}
			this->RenderSubMixed(&v_fblgr);
		}
		if (this->windows[A2VIDEO_DLGR].IsEnabled())
		{
			for (size_t i = 0; i < _A2VIDEO_TEXT_SIZE; i++)
			{
				// TODO: There should be no DLORES page 2
				this->UpdateDLoResRGBCell(
					(isPage2 ? _A2VIDEO_TEXT2_START : _A2VIDEO_TEXT1_START) + i,
					(isPage2 ? _A2VIDEO_TEXT2_START : _A2VIDEO_TEXT1_START),
					&v_fbdlgr);
			}
			this->RenderSubMixed(&v_fbdlgr);
		}
		if (this->windows[A2VIDEO_HGR].IsEnabled())
		{
			for (size_t i = 0; i < _A2VIDEO_HGR_SIZE; i++)
			{
				this->UpdateHiResRGBCell(
					(isPage2 ? _A2VIDEO_HGR2_START : _A2VIDEO_HGR1_START) + i,
					(isPage2 ? _A2VIDEO_HGR2_START : _A2VIDEO_HGR1_START),
					&v_fbhgr);
			}
			this->RenderSubMixed(&v_fbhgr);
		}
		if (this->windows[A2VIDEO_DHGR].IsEnabled())
		{
			for (size_t i = 0; i < _A2VIDEO_HGR_SIZE; i++)
			{
				this->UpdateDHiResRGBCell(
					(isPage2 ? _A2VIDEO_HGR2_START : _A2VIDEO_HGR1_START) + i,
					(isPage2 ? _A2VIDEO_HGR2_START : _A2VIDEO_HGR1_START),
					&v_fbdhgr);
			}
			this->RenderSubMixed(&v_fbdhgr);
		}
	}
ENDRENDER:
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL draw error: " << glerr << std::endl;
	}
	oglh->finalize_render();
}

void A2VideoManager::RenderSubMixed(std::vector<uint32_t>* framebuffer)
{
	// SHR overrides all other modes, no MIXED mode either
	if (IsSoftSwitch(A2SS_SHR))
	{
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
			_A2VIDEO_SHR_WIDTH, _A2VIDEO_SHR_HEIGHT,
			0, GL_RGBA, GL_UNSIGNED_BYTE, (void*)(&framebuffer->at(0)));
		return;
	}
	
	if (IsSoftSwitch(A2SS_MIXED))
		glTexSubImage2D(GL_TEXTURE_2D, 0,
			0, 0, _A2VIDEO_MIN_WIDTH, _A2VIDEO_MIN_MIXED_HEIGHT,
			GL_RGBA, GL_UNSIGNED_BYTE, (void*)(&framebuffer->at(0)));
	else
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
			_A2VIDEO_MIN_WIDTH, _A2VIDEO_MIN_HEIGHT,
			0, GL_RGBA, GL_UNSIGNED_BYTE, (void*)(&framebuffer->at(0)));
}

///////////////////////////////////////////////////////////////////////////////
// CPU-DRIVEN ORIGINAL A2 MODES GRAPHICS METHODS
// RGB Videocard code from AppleWin
///////////////////////////////////////////////////////////////////////////////

void A2VideoManager::UpdateSHRLine(uint8_t line_number, std::vector<uint32_t>* framebuffer)
{
	// SHR is all in AUX (E1) Bank
	uint8_t* pLine = SDHRManager::GetInstance()->GetApple2MemAuxPtr() + _A2VIDEO_SHR_START + line_number * _A2VIDEO_SHR_BYTES_PER_LINE;
	uint32_t* pVideoAddress = &framebuffer->at(line_number * _A2VIDEO_SHR_WIDTH * 2);
	uint32_t shrByte;

	const uint32_t k_shr_colors_per_palette = 16;
	const uint32_t k_shr_color_size = 2;

	// Grab Scanline Control Byte information
	bool _is640Mode;
	bool _isColorFillMode;
	uint8_t* _pscb = SDHRManager::GetInstance()->GetApple2MemAuxPtr() + _A2VIDEO_SHR_SCB_START;
	uint8_t _scb = *(_pscb + line_number);	// the scan control byte value
	_is640Mode = _scb & 0x80;
	_isColorFillMode = _scb & 0x20;
	// A palette is a 16-entry array of 4-bit-per-channel RGB colors (last 4 bits are reserved)
	// So each palette has 16 colors from any of 4096 colors
	uint16_t* palette = (uint16_t*)(SDHRManager::GetInstance()->GetApple2MemAuxPtr() +
		_A2VIDEO_SHR_PALETTE_START +
		((_scb & 0xf) * k_shr_colors_per_palette * k_shr_color_size)
		);
	// Now that we have the scanline control byte data, iterate through each pixel
	for (uint8_t i = 0; i < _A2VIDEO_SHR_BYTES_PER_LINE; i++)
	{
		shrByte = *reinterpret_cast<uint32_t*>(pLine + i);
		if (!_is640Mode) // 320 mode, 2 pixels per byte. Can use colorfill.
		{
			uint8_t pixel1 = (shrByte >> 4) & 0xf;
			uint32_t color1 = this->ConvertIIgs2RGB(palette[pixel1]);
			if (_isColorFillMode && pixel1 == 0 && (i > 0))
				color1 = *(pVideoAddress - 1);
			*pVideoAddress++ = color1;
			*pVideoAddress++ = color1;

			uint8_t pixel2 = shrByte & 0xf;
			uint32_t color2 = this->ConvertIIgs2RGB(palette[pixel2]);
			if (_isColorFillMode && pixel2 == 0)
				color2 = color1;
			*pVideoAddress++ = color2;
			*pVideoAddress++ = color2;
		}
		else // 640 mode, 4 pixels per byte. Cannot use colorfill.
		{
			uint8_t pixel1 = (shrByte >> 6) & 0x3;
			uint32_t color1 = this->ConvertIIgs2RGB(palette[0x8 + pixel1]);
			*pVideoAddress++ = color1;

			uint8_t pixel2 = (shrByte >> 4) & 0x3;
			uint32_t color2 = this->ConvertIIgs2RGB(palette[0xC + pixel2]);
			*pVideoAddress++ = color2;

			uint8_t pixel3 = (shrByte >> 2) & 0x3;
			uint32_t color3 = this->ConvertIIgs2RGB(palette[0x0 + pixel3]);
			*pVideoAddress++ = color3;

			uint8_t pixel4 = shrByte & 0x3;
			uint32_t color4 = this->ConvertIIgs2RGB(palette[0x4 + pixel4]);
			*pVideoAddress++ = color4;
		}
		
			// duplicate on the next row (it may be overridden by the scanlines)
		for (size_t j = 4; j >0; j--)
		{
			*(pVideoAddress - j + _A2VIDEO_SHR_WIDTH) = *(pVideoAddress - j);
		}
	}
}

const uint16_t BLUE_MASK = 0x000F;        // 0000 0000 0000 1111
const uint16_t GREEN_MASK = 0x00F0;       // 0000 0000 1111 0000
const uint16_t RED_MASK = 0x0F00;         // 0000 1111 0000 0000
// the 4 high bits are reserved

const uint16_t BLUE_SHIFT = 0;
const uint16_t GREEN_SHIFT = 4;
const uint16_t RED_SHIFT = 8;

uint32_t A2VideoManager::ConvertIIgs2RGB(uint16_t color)
{
	uint8_t red = (color & RED_MASK) >> RED_SHIFT;
	uint8_t green = (color & GREEN_MASK) >> GREEN_SHIFT;
	uint8_t blue = (color & BLUE_MASK) >> BLUE_SHIFT;
	uint8_t alpha = 0xFF; // Fully opaque

	// Scale up from 4 bits to 8 bits
	red *= 16;
	green *= 16;
	blue *= 16;

	// Combine into a 32-bit value (RGBA format)
	uint32_t rgba = (alpha << 24) | (blue << 16) | (green << 8) | red;

	return rgba;
}

void A2VideoManager::UpdateLoResRGBCell(uint16_t addr, const uint16_t addr_start, std::vector<uint32_t>* framebuffer)
{
	int x = LGR_ADDR2X[addr - addr_start];	// x start in pixels
	int y = LGR_ADDR2Y[addr - addr_start];	// y in pixels
	if (x < 0 || y < 0)	// the holes!
		return;

	// Everything is double the resolution
	x *= 2;
	y *= 2;
	uint8_t val = *(SDHRManager::GetInstance()->GetApple2MemPtr() + addr);

	uint8_t colorIdx;
	// Set all 14 dots in the top 4 rows for the low 4 bits color
	// and the bottom 4 rows for the high bits color
	// Duplicate each row for the double resolution rows
	for (size_t j = 0; j < 8; j++)
	{
		colorIdx = (j < 4) ? (val & 0xF) : (val & 0xF0) >> 4;
		for (size_t i = 0; i < 14; i++)
		{
			framebuffer->at(y * _A2VIDEO_MIN_WIDTH + x + i) = gPaletteRGB[colorIdx + 12];	// LoRes colors start at index 12
			framebuffer->at((y + 1) * _A2VIDEO_MIN_WIDTH + x + i) = gPaletteRGB[colorIdx + 12];
		}
		y += 2;
	}
}

#define ROL_NIB(x) ( (((x)<<1)&0xF) | (((x)>>3)&1) )

void A2VideoManager::UpdateDLoResRGBCell(uint16_t addr, const uint16_t addr_start, std::vector<uint32_t>* framebuffer)
{
	int x = LGR_ADDR2X[addr - addr_start];	// x start in pixels
	int y = LGR_ADDR2Y[addr - addr_start];	// y in pixels
	if (x < 0 || y < 0)	// the holes!
		return;

	// Everything is double the resolution
	x *= 2;
	y *= 2;

	uint8_t mainval = *(SDHRManager::GetInstance()->GetApple2MemPtr() + addr);
	uint8_t auxval = *(SDHRManager::GetInstance()->GetApple2MemAuxPtr() + addr);

	const uint8_t auxval_h = auxval >> 4;
	const uint8_t auxval_l = auxval & 0xF;
	auxval = (ROL_NIB(auxval_h) << 4) | ROL_NIB(auxval_l);

	uint8_t colorIdx;
	// Set all 7 dots of aux mem in the top 4 rows for the low 4 bits color
	// and the bottom 4 rows for the high bits color
	// Duplicate each row for the double resolution rows
	// And do it again for the 7 dots of main mem
	for (size_t j = 0; j < 8; j++)
	{
		colorIdx = (j < 4) ? (auxval & 0xF) : (auxval & 0xF0) >> 4;
		for (size_t i = 0; i < 7; i++)
		{
			framebuffer->at(y * _A2VIDEO_MIN_WIDTH + x + i) = gPaletteRGB[colorIdx + 12];	// LoRes colors start at index 12
			framebuffer->at((y + 1) * _A2VIDEO_MIN_WIDTH + x + i) = gPaletteRGB[colorIdx + 12];
		}
		colorIdx = (j < 4) ? (mainval & 0xF) : (mainval & 0xF0) >> 4;
		for (size_t i = 7; i < 14; i++)
		{
			framebuffer->at(y * _A2VIDEO_MIN_WIDTH + x + i) = gPaletteRGB[colorIdx + 12];	// LoRes colors start at index 12
			framebuffer->at((y + 1) * _A2VIDEO_MIN_WIDTH + x + i) = gPaletteRGB[colorIdx + 12];
		}

		y += 2;
	}
}

// Updates a single cell given a memory byte change
// The "cell" is 7 consecutive bits
void A2VideoManager::UpdateHiResRGBCell(uint16_t addr, const uint16_t addr_start, std::vector<uint32_t>* framebuffer)
{
	// first get the number of bytes from the start of the lines, i.e. the xb value
	int x = HGR_ADDR2X[addr - addr_start];	// x start in pixels
	int y = HGR_ADDR2Y[addr - addr_start];	// y in pixels
	if (x < 0 || y < 0)	// the holes!
		return;
	uint8_t xb = x / 7;	// x in bytes
	uint8_t xoffset = xb & 1; // offset to start of the 2 bytes. Always start with the even byte
	addr -= xoffset;
	x = HGR_ADDR2X[addr - addr_start];
	// Everything is double the resolution
	x *= 2;
	y *= 2;

	uint8_t* pMain = SDHRManager::GetInstance()->GetApple2MemPtr() + addr;

	// We need all 28 bits because each pixel needs a three bit evaluation
	// Anything outside the bounds of the row is 0
	uint8_t byteval1 = (xb < 2 ? 0 : *(pMain - 1));
	uint8_t byteval2 = *pMain;
	uint8_t byteval3 = *(pMain + 1);
	uint8_t byteval4 = (xb >= 38 ? 0 : *(pMain + 2));

	// all 28 bits chained
	uint32_t dwordval = (byteval1 & 0x7F) | ((byteval2 & 0x7F) << 7) | ((byteval3 & 0x7F) << 14) | ((byteval4 & 0x7F) << 21);

	// Extraction of 14 color pixels
	uint32_t colors[14];
	int color = 0;
	uint32_t dwordval_tmp = dwordval;
	dwordval_tmp = dwordval_tmp >> 7;
	bool offset = (byteval2 & 0x80) ? true : false;
	for (int i = 0; i < 14; i++)
	{
		if (i == 7) offset = (byteval3 & 0x80) ? true : false;
		color = dwordval_tmp & 0x3;
		// Two cases for the two palettes
		if (offset)
			colors[i] = gPaletteRGB[1 + color];
		else
			colors[i] = gPaletteRGB[6 - color];
		if (i % 2) dwordval_tmp >>= 2;
	}
	// Black and White
	uint32_t bw[2];
	bw[0] = gPaletteRGB[0];
	bw[1] = gPaletteRGB[1];

	uint32_t mask = 0x01C0; //  00|000001 1|1000000
	uint32_t chck1 = 0x0140; //  00|000001 0|1000000
	uint32_t chck2 = 0x0080; //  00|000000 1|0000000

	// To remove bleed when a pixel is between 2 white pixels
	uint32_t mask0 = 0b0000001111100000;
	uint32_t chck01 = 0b0000001101100000;

	// HIRES render in RGB works on a pixel-basis (1-bit data in framebuffer)
	// The pixel can be 'color', if it makes a 101 or 010 pattern with the two neighbour bits
	// In all other cases, it's black if 0 and white if 1
	// The value of 'color' is defined on a 2-bits basis

	if (xoffset)
	{
		// Second byte of the 14 pixels block
		dwordval = dwordval >> 7;
		xoffset = 7;
	}

	uint32_t dst = (y * _A2VIDEO_MIN_WIDTH) + x + (xoffset * 2);	// destination offset in the pixel framebuffer

	for (int i = xoffset; i < xoffset + 7; i++)
	{
		// remove bleed if a 0 pixel is between 2 white pixels ( 11 0 11 )
		if ((dwordval & mask0) == chck01)
		{
			framebuffer->at(dst) = bw[0];
			framebuffer->at(dst+1) = bw[0];
			dst += 2;
		}
		else if (((dwordval & mask) == chck1) || ((dwordval & mask) == chck2))
		{
			// Color pixel
			framebuffer->at(dst) = colors[i];
			framebuffer->at(dst+1) = colors[i];
			dst += 2;
		}
		else
		{
			// B&W pixel
			framebuffer->at(dst) = bw[(dwordval & chck2 ? 1 : 0)];
			framebuffer->at(dst + 1) = framebuffer->at(dst);
			dst += 2;
		}
		// Next pixel
		dwordval = dwordval >> 1;
	}
	// duplicate on the next row (it may be overridden by the scanlines)
	for (size_t i = 0; i < _A2VIDEO_MIN_WIDTH; i++)
	{
		framebuffer->at((y + 1) * _A2VIDEO_MIN_WIDTH + i) = framebuffer->at(y * _A2VIDEO_MIN_WIDTH + i);
	}
}

void A2VideoManager::UpdateDHiResRGBCell(uint16_t addr, const uint16_t addr_start, std::vector<uint32_t>* framebuffer)
{
	// first get the number of bytes from the start of the lines, i.e. the xb value
	int x = HGR_ADDR2X[addr - addr_start];	// x start in pixels
	int y = HGR_ADDR2Y[addr - addr_start];	// y in pixels
	if (x < 0 || y < 0)	// the holes!
		return;
	uint8_t xb = x / 7;	// x in bytes
	uint8_t xoffset = xb & 1; // offset to start of the 2 bytes. Always start with the even byte
	addr -= xoffset;
	x = HGR_ADDR2X[addr - addr_start];
	// Everything is double the resolution
	x *= 2;
	y *= 2;
	
	uint8_t* pMain = SDHRManager::GetInstance()->GetApple2MemPtr() + addr;
	uint8_t* pAux = SDHRManager::GetInstance()->GetApple2MemAuxPtr() + addr;
	
	// We need all 28 bits because one 4-bits pixel overlaps two 14-bits cells
	uint8_t byteval1 = *pAux;
	uint8_t byteval2 = *pMain;
	uint8_t byteval3 = *(pAux + 1);
	uint8_t byteval4 = *(pMain + 1);
	
	// all 28 bits chained
	uint32_t dwordval = (byteval1 & 0x7F) | ((byteval2 & 0x7F) << 7) | ((byteval3 & 0x7F) << 14) | ((byteval4 & 0x7F) << 21);
	
	// Extraction of 7 color pixels and 7x4 bits
	int bits[7];
	uint32_t colors[7];
	int color = 0;
	uint32_t dwordval_tmp = dwordval;
	for (int i = 0; i < 7; i++)
	{
		bits[i] = dwordval_tmp & 0xF;
		color = ((bits[i] & 7) << 1) | ((bits[i] & 8) >> 3); // DHGR colors are rotated 1 bit to the right
		colors[i] = *reinterpret_cast<const uint32_t*>(&gPaletteRGB[12 + color]);
		dwordval_tmp >>= 4;
	}
	
	// black and white colors
	uint32_t bw[2];
	bw[0] = gPaletteRGB[0];
	bw[1] = gPaletteRGB[1];
	
	// destination offset in the pixel framebuffer
	// We process a complete byte very time, so the offset for even/odd is 7 pixels * 2
	uint32_t dst = (y * _A2VIDEO_MIN_WIDTH) + x + (xoffset * 14);
	uint32_t* pDst = &framebuffer->at(dst);
	
	if (xoffset == 0)	// First cell
	{
		if (IsSoftSwitch(A2SS_DHGRMONO))	// Is Black and White
		{
			for (int i = 0; i < 14; i++)
			{
				*(pDst++) = bw[dwordval & 1];
				dwordval >>= 1;
			}
		} else {
			// Color cell 0
			*(pDst++) = colors[0];
			*(pDst++) = colors[0];
			*(pDst++) = colors[0];
			*(pDst++) = colors[0];
			// Color cell 1
			*(pDst++) = colors[1];
			*(pDst++) = colors[1];
			*(pDst++) = colors[1];
			
			// Remaining of color cell 1
			*(pDst++) = colors[1];
			
			// Color cell 2
			*(pDst++) = colors[2];
			*(pDst++) = colors[2];
			*(pDst++) = colors[2];
			*(pDst++) = colors[2];
			// Color cell 3
			*(pDst++) = colors[3];
			*(pDst++) = colors[3];
		}
	}
	else  // Second cell
	{
		if (IsSoftSwitch(A2SS_DHGRMONO))	// Is Black and White
		{
			dwordval >>= 14;
			for (int i = 0; i < 14; i++)
			{
				*(pDst++) = bw[dwordval & 1];
				dwordval >>= 1;
			}
		} else {
			// Remaining of color cell 3
			*(pDst++) = colors[3];
			*(pDst++) = colors[3];
			
			// Color cell 4
			*(pDst++) = colors[4];
			*(pDst++) = colors[4];
			*(pDst++) = colors[4];
			*(pDst++) = colors[4];
			// Color cell 5
			*(pDst++) = colors[5];
			
			// Remaining of color cell 5
			*(pDst++) = colors[5];
			*(pDst++) = colors[5];
			*(pDst++) = colors[5];
			
			// Color cell 6
			*(pDst++) = colors[6];
			*(pDst++) = colors[6];
			*(pDst++) = colors[6];
			*(pDst++) = colors[6];
		}
	}
	
	// duplicate on the next row (it may be overridden by the scanlines)
	for (size_t i = 0; i < _A2VIDEO_MIN_WIDTH; i++)
	{
		framebuffer->at((y + 1) * _A2VIDEO_MIN_WIDTH + i) = framebuffer->at(y * _A2VIDEO_MIN_WIDTH + i);
	}
}


