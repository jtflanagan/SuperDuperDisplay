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
#include <iostream>
#include <iomanip>
#include "OpenGLHelper.h"
#include "MemoryManager.h"
#include "EventRecorder.h"
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

// Memory offsets for TEXT/D/LGR and D/HGR modes
// The rows aren't contiguous in Apple 2 RAM.
// They're interlaced because WOZ chip optimization.

// Apple 2 TEXT row offset interlacing in RAM
static uint16_t g_RAM_TEXTOffsets[] =
{
	0x0000, 0x0080, 0x0100, 0x0180, 0x0200, 0x0280, 0x0300, 0x0380,
	0x0028, 0x00A8, 0x0128, 0x01A8, 0x0228, 0x02A8, 0x0328, 0x03A8,
	0x0050, 0x00D0, 0x0150, 0x01D0, 0x0250, 0x02D0, 0x0350, 0x03D0
};
// Apple 2 HGR row offsets interlacing in RAM
static uint16_t g_RAM_HGROffsets[] = {
	0x0000, 0x0400, 0x0800, 0x0C00, 0x1000, 0x1400, 0x1800, 0x1C00,
	0x0080, 0x0480, 0x0880, 0x0C80, 0x1080, 0x1480, 0x1880, 0x1C80,
	0x0100, 0x0500, 0x0900, 0x0D00, 0x1100, 0x1500, 0x1900, 0x1D00,
	0x0180, 0x0580, 0x0980, 0x0D80, 0x1180, 0x1580, 0x1980, 0x1D80,
	0x0200, 0x0600, 0x0A00, 0x0E00, 0x1200, 0x1600, 0x1A00, 0x1E00,
	0x0280, 0x0680, 0x0A80, 0x0E80, 0x1280, 0x1680, 0x1A80, 0x1E80,
	0x0300, 0x0700, 0x0B00, 0x0F00, 0x1300, 0x1700, 0x1B00, 0x1F00,
	0x0380, 0x0780, 0x0B80, 0x0F80, 0x1380, 0x1780, 0x1B80, 0x1F80,
	0x0028, 0x0428, 0x0828, 0x0C28, 0x1028, 0x1428, 0x1828, 0x1C28,
	0x00A8, 0x04A8, 0x08A8, 0x0CA8, 0x10A8, 0x14A8, 0x18A8, 0x1CA8,
	0x0128, 0x0528, 0x0928, 0x0D28, 0x1128, 0x1528, 0x1928, 0x1D28,
	0x01A8, 0x05A8, 0x09A8, 0x0DA8, 0x11A8, 0x15A8, 0x19A8, 0x1DA8,
	0x0228, 0x0628, 0x0A28, 0x0E28, 0x1228, 0x1628, 0x1A28, 0x1E28,
	0x02A8, 0x06A8, 0x0AA8, 0x0EA8, 0x12A8, 0x16A8, 0x1AA8, 0x1EA8,
	0x0328, 0x0728, 0x0B28, 0x0F28, 0x1328, 0x1728, 0x1B28, 0x1F28,
	0x03A8, 0x07A8, 0x0BA8, 0x0FA8, 0x13A8, 0x17A8, 0x1BA8, 0x1FA8,
	0x0050, 0x0450, 0x0850, 0x0C50, 0x1050, 0x1450, 0x1850, 0x1C50,
	0x00D0, 0x04D0, 0x08D0, 0x0CD0, 0x10D0, 0x14D0, 0x18D0, 0x1CD0,
	0x0150, 0x0550, 0x0950, 0x0D50, 0x1150, 0x1550, 0x1950, 0x1D50,
	0x01D0, 0x05D0, 0x09D0, 0x0DD0, 0x11D0, 0x15D0, 0x19D0, 0x1DD0,
	0x0250, 0x0650, 0x0A50, 0x0E50, 0x1250, 0x1650, 0x1A50, 0x1E50,
	0x02D0, 0x06D0, 0x0AD0, 0x0ED0, 0x12D0, 0x16D0, 0x1AD0, 0x1ED0,
	0x0350, 0x0750, 0x0B50, 0x0F50, 0x1350, 0x1750, 0x1B50, 0x1F50,
	0x03D0, 0x07D0, 0x0BD0, 0x0FD0, 0x13D0, 0x17D0, 0x1BD0, 0x1FD0
};

// Gotta have a transparency global just in case
// static uint32_t gRGBTransparent = 0;

// below because "The declaration of a static data member in its class definition is not a definition"
A2VideoManager* A2VideoManager::s_instance;

static OpenGLHelper* oglHelper;


//////////////////////////////////////////////////////////////////////////
// Image Asset Methods
//////////////////////////////////////////////////////////////////////////

// NOTE:	Both the below image asset methods use OpenGL 
//			so they _must_ be called from the main thread
void A2VideoManager::ImageAsset::AssignByFilename(A2VideoManager* owner, const char* filename) {
	(void)owner; // mark as unused
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
	// Here do not reinitialize bBeamIsActive. It could still be active from earlier.
	// The initialization process can be triggered from a ctrl-reset on the Apple 2.

	oglHelper = OpenGLHelper::GetInstance();
	bIsReady = false;
	current_frame_idx = 0;
	for (int i = 0; i < 2; i++)
	{
		vrams_array[i].id = i;
		vrams_array[i].frame_idx = 0;
		vrams_array[i].mode = A2Mode_e::SHR;
		if (vrams_array[i].vram_legacy != nullptr)
			delete[] vrams_array[i].vram_legacy;
		if (vrams_array[i].vram_shr != nullptr)
			delete[] vrams_array[i].vram_shr;
		vrams_array[i].vram_legacy = new uint8_t[GetVramSizeLegacy()];
		vrams_array[i].vram_shr = new uint8_t[GetVramSizeSHR()];
		memset(vrams_array[i].vram_legacy, 0, GetVramSizeLegacy());
		memset(vrams_array[i].vram_shr, 0, GetVramSizeSHR());
	}
	vrams_write = &vrams_array[current_frame_idx % 2];
	vrams_read = &vrams_array[1 - (current_frame_idx % 2)];
	
	auto memMgr = MemoryManager::GetInstance();

	// Set up the image assets (textures)
	// Assign them their respective GPU texture id
	*image_assets = {};
	for (uint8_t i = 0; i < (sizeof(image_assets) / sizeof(ImageAsset)); i++)
	{
		image_assets[i].tex_id = oglHelper->get_texture_id_at_slot(i);
	}

	// Initialize windows
	windowsbeam[A2VIDEOBEAM_LEGACY] = std::make_unique<A2WindowBeam>(A2VIDEOBEAM_LEGACY, _SHADER_A2_VERTEX_DEFAULT, _SHADER_BEAM_LEGACY_FRAGMENT);
	windowsbeam[A2VIDEOBEAM_SHR] = std::make_unique<A2WindowBeam>(A2VIDEOBEAM_SHR, _SHADER_A2_VERTEX_DEFAULT, _SHADER_BEAM_SHR_FRAGMENT);
	windowsbeam[A2VIDEOBEAM_LEGACY]->SetBorder(borders_w_cycles, borders_h_scanlines);
	windowsbeam[A2VIDEOBEAM_SHR]->SetBorder(borders_w_cycles, borders_h_scanlines);

	// The merged framebuffer will have a size equal to the SHR buffer (including borders)
	fb_width = windowsbeam[A2VIDEOBEAM_SHR]->GetWidth();
	fb_height = windowsbeam[A2VIDEOBEAM_SHR]->GetHeight();

	// We don't know the beam state
	// Wait until beam is at position (0,0) to start
	beamState = BeamState_e::UNKNOWN;

	// TODO: 	SET THE OUTPUT TEXTURE FOR EACH OF THE 2 WINDOWSBEAM
	//			SO WE CAN MERGE THE 2 AFTERWARDS

	// tell the next Render() call to run initialization routines
	bShouldInitializeRender = true;
	
	bIsReady = true;
}

A2VideoManager::~A2VideoManager()
{
	for (int i = 0; i < 2; i++)
	{
		if (vrams_array[i].vram_legacy != nullptr)
			delete[] vrams_array[i].vram_legacy;
		if (vrams_array[i].vram_shr != nullptr)
			delete[] vrams_array[i].vram_shr;
	}
	delete[] vrams_array;
}

void A2VideoManager::ResetComputer()
{
    if (bIsRebooting == true)
        return;
    bIsRebooting = true;
	MemoryManager::GetInstance()->Initialize();
    bIsRebooting = false;
}

bool A2VideoManager::IsReady()
{
	return bIsReady;
}

void A2VideoManager::ToggleA2Video(bool value)
{
	bA2VideoEnabled = value;
	if (bA2VideoEnabled)
		bShouldInitializeRender = true;
}

void A2VideoManager::SetBordersWithReinit(uint8_t width_cycles, uint8_t height_8s)
{
	if (width_cycles > 7)
		width_cycles = 7;
	if (height_8s > 3)
		height_8s = 3;
	borders_w_cycles = width_cycles;
	borders_h_scanlines = height_8s * 8;	// Must be multiple of 8s
	auto _mms = MemoryManager::GetInstance()->SerializeSwitches();
	this->Initialize();
	MemoryManager::GetInstance()->DeserializeSwitches(_mms);
	this->ForceBeamFullScreenRender();
}


void A2VideoManager::StartNextFrame()
{
	// start the next frame
	// set the frame index for the buffer we'll move to reading
	vrams_write->frame_idx = current_frame_idx;
	++current_frame_idx;
	// Flip the double buffers
	if (vrams_read->bWasRendered)
	{
		vrams_read->bWasRendered = false;
		auto _vtmp = vrams_write;
		vrams_write = vrams_read;
		vrams_read = _vtmp;
	}
	// reset the mode at each vblank
	vrams_write->mode = A2Mode_e::NONE;
	// Update the current region info
	current_region = CycleCounter::GetInstance()->GetVideoRegion();
	region_scanlines = (current_region == VideoRegion_e::NTSC ? SC_TOTAL_NTSC : SC_TOTAL_PAL);
}

void A2VideoManager::BeamIsAtPosition(uint32_t _x, uint32_t _y)
{
	/*
		@: Frame flip and start of next frame
		&: Start next frame scanlines
	 ||H|        |H||----------------------------------------------------------------------------|
	 ||B|        |B||                                                                      		 |
	 ||o|        |o||                                                                      		 |
	 ||r| HBLANK |r||                                    Content                                 |
	 ||d|        |d||                     CYCLES_SC_CONTENT x mode_scanlines                  	 |
	 ||e|        |e||                                                                      		 |
	 ||r|        |r||                                                                      		 |
	 |		        |----------------------         Vertical border       -----------------------|
	 |		        |------------------------- (borders_h_scanlines) -------------------------|
	 |@		        |............................. vertical blanking ............................|
	 |		        |............................. vertical blanking ............................|
	 |		        |............................. vertical blanking ............................|
	 |		        |............................. vertical blanking ............................|
	 |		        |............................. vertical blanking ............................|
	 |		        |............................. vertical blanking ............................|
	 |		        |............................. vertical blanking ............................|
	 |&             |-----------------      Vertical border  (next frame)       -----------------|
	 |		        |------------------------- (borders_h_scanlines) -------------------------|

	 In order to achieve a "correct" top, left, right, bottom border around the content, with
	 the origin being at the start of the top border, we translate each border area's x & w
	 with the below #defines for each of Border L, R, T, B.
	 */

#define _TR_ANY_X ((_x + borders_w_cycles + CYCLES_SC_CONTENT) % CYCLES_SC_TOTAL)
#define _TR_ANY_Y ((_y + borders_h_scanlines) % region_scanlines)

	if (!bIsReady)
		return;

	auto memMgr = MemoryManager::GetInstance();
	uint32_t mode_scanlines = (memMgr->IsSoftSwitch(A2SS_SHR) ? 200 : 192);

	// The Apple 2gs drawing is shifted 6 scanlines down
	// Let's realign it to match the 2e
	if (memMgr->is2gs)
	{
		_y = (_y + region_scanlines - 6) % region_scanlines;
	}

	// Do not bother with the beam state until we get a 0,0
	// Then we can start doing work
	if (_x == 0 && _y == 0)	// initialize
	{
		beamState = BeamState_e::BORDER_RIGHT;
	}

	// Now determine the actual beam state
	// And flip the frame when switching from BORDER_BOTTOM to NBVBLANK
	// keep updating the beam state until it reaches steady state
	auto _oldBeamState = beamState;

	while (true)
	{
		switch (beamState)
		{
		case BeamState_e::UNKNOWN:
			break;
		case BeamState_e::NBHBLANK:
			if (_x == (CYCLES_SC_HBL - borders_w_cycles))
				beamState = BeamState_e::BORDER_LEFT;
			break;
		case BeamState_e::NBVBLANK:
			if (_y == (region_scanlines - borders_h_scanlines))
				beamState = BeamState_e::BORDER_TOP;
			break;
		case BeamState_e::BORDER_LEFT:
			if (_x == CYCLES_SC_HBL)
				beamState = BeamState_e::CONTENT;
			break;
		case BeamState_e::BORDER_RIGHT:
			if (_x == borders_w_cycles)
				beamState = BeamState_e::NBHBLANK;
			break;
		case BeamState_e::BORDER_TOP:
			if (_y == 0)
				beamState = BeamState_e::BORDER_RIGHT;
			break;
		case BeamState_e::BORDER_BOTTOM:
			if (_y == (mode_scanlines + borders_h_scanlines))
			{
				beamState = BeamState_e::NBVBLANK;
				// Start of NBVBLANK at which we flip the double buffering
				StartNextFrame();
			}
			break;
		case BeamState_e::CONTENT:
			if (_x == 0)
			{
				if (_y == mode_scanlines)
					beamState = BeamState_e::BORDER_BOTTOM;
				else
					beamState = BeamState_e::BORDER_RIGHT;
			}
			break;
		default:
			break;
		}
		if (_oldBeamState == beamState)
			break;
		_oldBeamState = beamState;
	}

	// Now we get rid of all the non-border BLANK areas to avoid an overflow on the vertical border areas.
	// We never want to process vertical borders that are in non-border HBLANK
	// Otherwise we'd need the vertical border states to know when they're in HBLANK as well,
	// complicating the state machine.

	if (_x >= borders_w_cycles && _x < (CYCLES_SC_HBL - borders_w_cycles))
		return;
	if (_y >= (mode_scanlines + borders_h_scanlines) && _y < (region_scanlines - borders_h_scanlines))
		return;

	// Always at the start of the row, set the SHR SCB to 0x10
	// Because we check bit 4 of the SCB to know if that line is drawn as SHR
	// The 2gs will always set bit 4 to 0 when sending it over
	if (_x == 0)
	{
		vrams_write->vram_shr[GetVramWidthSHR() * _TR_ANY_Y] = 0x10;
	}

	// Now generate the VRAMs themselves

	if (memMgr->IsSoftSwitch(A2SS_SHR))
	{
		// at least 1 byte in this vblank cycle is in SHR
		vrams_write->mode = (vrams_write->mode == A2Mode_e::LEGACY ? A2Mode_e::MIXED : A2Mode_e::SHR);

		auto memPtr = memMgr->GetApple2MemAuxPtr();
		uint8_t* lineStartPtr = vrams_write->vram_shr + GetVramWidthSHR() * _TR_ANY_Y;

		switch (beamState)
		{
		case BeamState_e::UNKNOWN:
			break;
		case BeamState_e::NBHBLANK:
			// do nothing
			break;
		case BeamState_e::NBVBLANK:
			// do nothing
			break;
		case BeamState_e::BORDER_LEFT:
			memset(lineStartPtr + _COLORBYTESOFFSET + _TR_ANY_X * 4, (uint8_t)memMgr->switch_c034, 4);
			// get the SCB and palettes if we're starting a line
			// and it's part of the content area. The top & bottom border areas don't care about SCB
			if ((_TR_ANY_X == 0) && (_y < mode_scanlines))
			{
				lineStartPtr[0] = memPtr[_A2VIDEO_SHR_SCB_START + _y];
				vrams_write->vram_shr[GetVramWidthSHR() * _TR_ANY_Y] = memPtr[_A2VIDEO_SHR_SCB_START + _y];
				// Get the palette
				memcpy(lineStartPtr + 1,	// palette starts at byte 1 in our a2shr_vram
					memPtr + _A2VIDEO_SHR_PALETTE_START + ((uint32_t)(lineStartPtr[0] & 0xFu) * 32),
					32);					// palette length is 32 bytes
			}
			break;
		case BeamState_e::BORDER_RIGHT:
		case BeamState_e::BORDER_TOP:
		case BeamState_e::BORDER_BOTTOM:
			memset(lineStartPtr + _COLORBYTESOFFSET + (_TR_ANY_X * 4), (uint8_t)memMgr->switch_c034, 4);
			break;
		case BeamState_e::CONTENT:
		{
			if (_x < CYCLES_SC_HBL || _y >= mode_scanlines)
			{
				// Somehow in the middle of the frame the mode was switched, and we're beyond the
				// legacy content area. Disregard.
				break;
			}
			// Get the color info for the 4 bytes where the beam is
			auto contentOffset = _COLORBYTESOFFSET + (borders_w_cycles * 4);
			auto xfb = (_x - CYCLES_SC_HBL) * 4;	// the x first byte, given that every beam cycle renders 4 bytes
			auto scb = lineStartPtr[0];
			memcpy(lineStartPtr + _COLORBYTESOFFSET + _TR_ANY_X * 4,
				memPtr + _A2VIDEO_SHR_START + _y * _A2VIDEO_SHR_BYTES_PER_LINE + xfb, 4);
			if (!(scb & 0x80u) && (scb & 0x20u))	// 320 mode and colorfill
			{
				// Pre-calculate colorfill, so that the shader doesn't have to do it
				// It's completely wasted on the shader. Here it's much more efficient
				for (uint32_t i = 0; i < 4; i++)
				{
					auto byteColor = lineStartPtr[_COLORBYTESOFFSET + (_TR_ANY_X * 4) + i];
					// if the first color of the byte is 0, give it the last color of the previous byte
					// assuming this is not the first byte of the line
					if (((byteColor & 0xF0) == 0) && ((xfb + i) != 0))
						byteColor |= (lineStartPtr[_COLORBYTESOFFSET + (_TR_ANY_X * 4) + i - 1] & 0b1111) << 4;
					// if the second color of the byte is 0, give it the first color of the byte
					if ((byteColor & 0x0F) == 0)
						byteColor |= (byteColor >> 4);
					lineStartPtr[_COLORBYTESOFFSET + (_TR_ANY_X * 4) + i] = byteColor;
				}
			}
		}
			break;
		default:
			break;
		}
		return;
	}	// if (memMgr->IsSoftSwitch(A2SS_SHR))


	// The byte isn't SHR, it's legacy
	// at least 1 byte in this vblank cycle is LEGACY
	vrams_write->mode = (vrams_write->mode == A2Mode_e::SHR ? A2Mode_e::MIXED : A2Mode_e::LEGACY);
	// the flags byte is:
	// bits 0-2: mode (TEXT, DTEXT, LGR, DLGR, HGR, DHGR, DHGRMONO, BORDER)
	// bit 3: ALT charset for TEXT
	// bits 4-7: border color (like in the 2gs)
	uint8_t flags = 0;
	// the colors byte is:
	// bits 0-3: background color
	// bits 4-7: foreground color
	uint8_t colors = 0;
	
	switch (beamState)
	{
	case BeamState_e::UNKNOWN:
		break;
	case BeamState_e::NBHBLANK:
		// do nothing
		break;
	case BeamState_e::NBVBLANK:
		// do nothing
		break;
	case BeamState_e::BORDER_LEFT:
	case BeamState_e::BORDER_RIGHT:
	case BeamState_e::BORDER_TOP:
	case BeamState_e::BORDER_BOTTOM:
		// Legacy mode VRAM is 4 bytes (main, aux, flags, fg&bg colors)
		// Set byte 3 as border color in the top 4 bits, and mode BORDER in the lower 3 bits
		vrams_write->vram_legacy[(GetVramWidthLegacy() * _TR_ANY_Y + _TR_ANY_X) * 4 + 2] =
			(memMgr->switch_c034 << 4) + 0b111;
		break;
	case BeamState_e::CONTENT:
	{
		if (_x < CYCLES_SC_HBL || _y >= mode_scanlines)
		{
			// Somehow in the middle of the frame the mode was switched, and we're beyond the
			// legacy content area. Disregard.
			break;
		}
		// Set the mode, and depending on the mode, grab the bytes
		if (!memMgr->IsSoftSwitch(A2SS_TEXT))
		{
			if (memMgr->IsSoftSwitch(A2SS_MIXED) && _y > 159)	// check mixed mode
			{
				if (memMgr->IsSoftSwitch(A2SS_80COL))
					flags = 1;	// DTEXT
				else
					flags = 0;	// TEXT
			}
			else if (memMgr->IsSoftSwitch(A2SS_80COL) && memMgr->IsSoftSwitch(A2SS_DHGR))	// double resolution
			{
				if (memMgr->IsSoftSwitch(A2SS_HIRES))
				{
					if (memMgr->IsSoftSwitch(A2SS_DHGRMONO))
						flags = 6;	// DHGRMONO
					else
						flags = 5;	// DHGR
				}
				else
					flags = 3;	// DLGR
			}
			else if (memMgr->IsSoftSwitch(A2SS_HIRES))	// standard hires
			{
				flags = 4;	// HGR
			}
			else {	// standard lores
				flags = 2;	// LGR
			}
		}
		else { 	// Now check the text modes
			if (memMgr->IsSoftSwitch(A2SS_80COL))
				flags = 1;	// DTEXT
			else
			{
				flags = 0;	// TEXT
			}
		}

		// Fill in the rest of the flags. We already use bits 0-2 for the modes
		flags |= ((memMgr->IsSoftSwitch(A2SS_ALTCHARSET) ? 1 : 0) << 3);	// bit 3 is alt charset
		flags |= (memMgr->switch_c034 << 4);								// bits 4-7 are border color
		// and the colors
		colors = memMgr->switch_c022;
		// Check for page 2
		bool isPage2 = false;
		// Careful: it's only page 2 if 80STORE is off
		if (memMgr->IsSoftSwitch(A2SS_PAGE2) && !memMgr->IsSoftSwitch(A2SS_80STORE))
			isPage2 = true;

		// Finally set the 4 VRAM bytes
		// 4 bytes in VRAM for each beam byte
		uint8_t* byteStartPtr = vrams_write->vram_legacy +
			(GetVramWidthLegacy() * _TR_ANY_Y + _TR_ANY_X) * 4;

		// Determine where in memory we should get the data from, and get it
		if ((flags & 0b111) < 4)	// D/TEXT AND D/LGR
		{
			uint32_t startMem = _A2VIDEO_TEXT1_START;
			if (((flags & 0b111) < 3) && isPage2)		// check for page 2 (DLGR doesn't have it)
				startMem = _A2VIDEO_TEXT2_START;
			byteStartPtr[0] = *(memMgr->GetApple2MemPtr() + startMem + g_RAM_TEXTOffsets[_y / 8] + (_x - CYCLES_SC_HBL));
			byteStartPtr[1] = *(memMgr->GetApple2MemAuxPtr() + startMem + g_RAM_TEXTOffsets[_y / 8] + (_x - CYCLES_SC_HBL));
		}
		else {		// D/HIRES
			uint32_t startMem = _A2VIDEO_HGR1_START;
			if (isPage2)
				startMem = _A2VIDEO_HGR2_START;
			byteStartPtr[0] = *(memMgr->GetApple2MemPtr() + startMem + g_RAM_HGROffsets[_y] + (_x - CYCLES_SC_HBL));
			byteStartPtr[1] = *(memMgr->GetApple2MemAuxPtr() + startMem + g_RAM_HGROffsets[_y] + (_x - CYCLES_SC_HBL));
		}
		byteStartPtr[2] = flags;
		byteStartPtr[3] = colors;
	}
		break;
	default:
		break;
	}

}

void A2VideoManager::ForceBeamFullScreenRender()
{
	// Move the beam over the whole screen
	auto totalscanlines = (current_region == VideoRegion_e::NTSC ? SC_TOTAL_NTSC : SC_TOTAL_PAL);
	// Start in the non-border VBLANK area to get all the borders
	int starty = SC_TOTAL_NTSC - borders_h_scanlines - 1;
	beamState = BeamState_e::NBVBLANK;
	// Run both frames
	for (size_t i = 0; i < 2; i++)
	{
		// Forces a full screen render for the beam renderer
		rendered_frame_idx = UINT64_MAX;
		for (uint32_t y = starty; y < totalscanlines; y++)
		{
			for (uint32_t x = 0; x < 65; x++)
			{
				this->BeamIsAtPosition(x, y);
			}
		}
		for (uint32_t y = 0; y < starty; y++)
		{
			for (uint32_t x = 0; x < 65; x++)
			{
				this->BeamIsAtPosition(x, y);
			}
		}
		this->Render();
		this->StartNextFrame();
	}

}

uXY A2VideoManager::ScreenSize()
{
	return uXY({ output_width, output_height});
}

GLuint A2VideoManager::Render()
{
	// We first render both the legacy and shr "windows" as textures
	// (which ever one of the two, or both, is enabled for this frame)
	// and then merge them into one using a merge shader. If only a single
	// mode is active, then just pass its output texture to the postprocessor
	// directly and avoid all the mess, unless the user has requested that
	// the legacy mode use 16MHz instead of 14MHz for width.

	// TODO: Make the merge shader that will go through each line
	//		and determine which mode it's on and load from the correct
	//		texture (legacy or shr) while also keeping track of the
	//		switch between modes, hence translating the line as necessary
	//		for the sine wobble effect

	// TODO: make the buffer that determines the translation effect

	if (!bIsReady)
		return UINT32_MAX;

	if (!bA2VideoEnabled)
		return UINT32_MAX;
	
	GLenum glerr;
	if (FBO_merged == UINT_MAX)
	{
		glGenFramebuffers(1, &FBO_merged);
		glGenTextures(1, &merged_texture_id);
		glActiveTexture(_TEXUNIT_POSTPROCESS);
		glBindFramebuffer(GL_FRAMEBUFFER, FBO_merged);
		glBindTexture(GL_TEXTURE_2D, merged_texture_id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fb_width, fb_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, merged_texture_id, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	// Initialization routine runs only once on init (or re-init)
	// We do that here because we know the framebuffer is bound, and everything
	// for drawing the SDHR stuff is active
	if (bShouldInitializeRender) {
		bShouldInitializeRender = false;
		glBindFramebuffer(GL_FRAMEBUFFER, FBO_merged);
		glClearColor(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);

		// image asset 0: The apple 2e US font
		glActiveTexture(_TEXUNIT_IMAGE_ASSETS_START);
		image_assets[0].AssignByFilename(this, "assets/Apple2eFont14x16 - Regular.png");
		// image asset 1: The alternate font
		glActiveTexture(_TEXUNIT_IMAGE_ASSETS_START + 1);
		image_assets[1].AssignByFilename(this, "assets/Apple2eFont14x16 - Alternate.png");
		// image asset 2: The apple 2e US font 80COL
		glActiveTexture(_TEXUNIT_IMAGE_ASSETS_START + 2);
		image_assets[2].AssignByFilename(this, "assets/Apple2eFont7x16 - Regular.png");
		// image asset 3: The alternate font 80COL
		glActiveTexture(_TEXUNIT_IMAGE_ASSETS_START + 3);
		image_assets[3].AssignByFilename(this, "assets/Apple2eFont7x16 - Alternate.png");
		// image asset 4: LGR texture (overkill for color, useful for dithered b/w)
		glActiveTexture(_TEXUNIT_IMAGE_ASSETS_START + 4);
		image_assets[4].AssignByFilename(this, "assets/Texture_composite_lgr.png");
		// image asset 5: HGR texture
		glActiveTexture(_TEXUNIT_IMAGE_ASSETS_START + 5);
		image_assets[5].AssignByFilename(this, "assets/Texture_composite_hgr.png");
		// image asset 6: DHGR texture
		glActiveTexture(_TEXUNIT_IMAGE_ASSETS_START + 6);
		image_assets[6].AssignByFilename(this, "assets/Texture_composite_dhgr.png");
		// image asset 7: The bezel for postprocessing
		glActiveTexture(_TEXUNIT_IMAGE_ASSETS_START + 7);
		image_assets[7].AssignByFilename(this, "assets/Bezel.png");
		if ((glerr = glGetError()) != GL_NO_ERROR) {
			std::cerr << "OpenGL AssignByFilename error: " 
				<< 0 << " - " << glerr << std::endl;
		}

		if ((glerr = glGetError()) != GL_NO_ERROR) {
			std::cerr << "OpenGL render A2VideoManager error: " << glerr << std::endl;
		}

		rendered_frame_idx = UINT64_MAX;
	}

	// Exit if we've already rendered the buffer
	if (rendered_frame_idx == vrams_read->frame_idx)
		return output_texture_id;

	glGetIntegerv(GL_VIEWPORT, last_viewport);	// remember existing viewport to restore it later
	// Now determine how we should merge both legacy and shr
	if (vrams_read->mode == A2Mode_e::MIXED)
	{
		// Both are active in this frame, we need to do the merge
		// TODO: TAKE BOTH OUTPUT TEXTURES AND BIND THEM TO TEXTURES 13 and 14
		// TODO: RENDER VIA A MERGE SHADER
		output_width = fb_width;
		output_height = fb_height;
		glViewport(0, 0, output_width, output_height);
		GLuint legacy_texture_id = windowsbeam[A2VIDEOBEAM_LEGACY]->Render(true);
		GLuint shr_texture_id = windowsbeam[A2VIDEOBEAM_SHR]->Render(true);
		output_texture_id = merged_texture_id;
	}
	else if (vrams_read->mode == A2Mode_e::LEGACY) {
		// Only legacy is active, just bind the correct output for the postprocessor
		output_width = windowsbeam[A2VIDEOBEAM_LEGACY]->GetWidth();
		output_height = windowsbeam[A2VIDEOBEAM_LEGACY]->GetHeight();
		glViewport(0, 0, output_width, output_height);
		output_texture_id = windowsbeam[A2VIDEOBEAM_LEGACY]->Render(true);
		// std::cerr << output_width << "x" << output_height << " - " << output_texture_id << std::endl;
	}
	else if (vrams_read->mode == A2Mode_e::SHR) {
		// Only SHR is active, just bind the correct output for the postprocessor
		output_width = windowsbeam[A2VIDEOBEAM_SHR]->GetWidth();
		output_height = windowsbeam[A2VIDEOBEAM_SHR]->GetHeight();
		glViewport(0, 0, output_width, output_height);
		output_texture_id = windowsbeam[A2VIDEOBEAM_SHR]->Render(true);
		// std::cerr << output_width << "x" << output_height << " - " << output_texture_id << std::endl;
	}
	glActiveTexture(_TEXUNIT_POSTPROCESS);
	glBindTexture(GL_TEXTURE_2D, output_texture_id);
	
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL draw error: " << glerr << std::endl;
	}

	// cleanup
	glActiveTexture(GL_TEXTURE0);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(last_viewport[0], last_viewport[1], (GLsizei)last_viewport[2], (GLsizei)last_viewport[3]);

	// all done, the texture for this Apple 2 beam cycle frame is rendered
	rendered_frame_idx = vrams_read->frame_idx;
	vrams_read->bWasRendered = true;
	return output_texture_id;
}

GLuint A2VideoManager::GetOutputTextureId()
{
	return output_texture_id;
}

void A2VideoManager::ActivateBeam()
{
	bBeamIsActive = true;
}

void A2VideoManager::DeactivateBeam()
{
	bBeamIsActive = false;
}
