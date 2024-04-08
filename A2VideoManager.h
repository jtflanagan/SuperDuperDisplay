#pragma once

#ifndef A2VIDEOMANAGER_H
#define A2VIDEOMANAGER_H

#include <stdint.h>
#include <stddef.h>

#include "common.h"
#include "A2WindowBeam.h"
#include "CycleCounter.h"

enum class A2Mode_e
{
	NONE = 0,
	LEGACY,
	SHR,
	MERGED,
	A2MODE_TOTAL_COUNT
};

enum class BeamState_e
{
	UNKNOWN = 0,
	NBHBLANK,			// HBLANK not in border
	NBVBLANK,			// VBLANK not in border
	BORDER_LEFT,
	BORDER_RIGHT,
	BORDER_TOP,
	BORDER_BOTTOM,
	CONTENT,
	BEAMSTATE_TOTAL_COUNT
};

// There could be anywhere up to 6 or 7 cycles for horizontal borders
// and a lot more for vertical borders.
// But SHR starts VBLANK just like legacy modes, at scanline 192. Hence
// it has 8 less bottom border scanlines than legacy.
// Vertical borders must be in multiples of 8 to ensure the shader works
// properly given the text modes
constexpr uint32_t _BORDER_WIDTH_MAX_CYCLES = 7;
constexpr uint32_t _BORDER_HEIGHT_MAX_MULT8 = 3;	// 3*8 scanlines

// Our arbitrary start of a new frame. It should be inside VBLANK and
// after the maximum bottom border size, but before the top border
constexpr uint32_t _SCANLINE_START_FRAME = 200 + (_BORDER_HEIGHT_MAX_MULT8 * 8) + 1;

// Legacy mode VRAM is 4 bytes (main, aux, flags, colors)
// for each "byte" of screen use
// colors are 4-bit each of fg and bg colors as in the Apple 2gs


// SHR mode VRAM is the standard bytes of screen use ($2000 to $9CFF)
// plus, for each of the 200 lines, at the beginning of the line draw
// (at end of HBLANK) the SCB (1 byte) and palette (32 bytes) of that line
// SHR mode VRAM looks like this:

// SCB        PALETTE                    COLOR BYTES
// [0 [(1 2) (3 4) ... (31 32)] [[L_BORDER] 0 ......... 159 [R_BORDER]]]	// line 0
// [0 [(1 2) (3 4) ... (31 32)] [[L_BORDER] 0 ......... 159 [R_BORDER]]]	// line 1
//                         .
//                         .
//                         .
//                         .
// [0 [(1 2) (3 4) ... (31 32)] [[L_BORDER] 0 ......... 159 [R_BORDER]]]	// line 199

// The BORDER bytes have the exact border color in their lower 4 bits
// Each SHR cycle is 4 bytes, and each byte is 4 pixels (2x2 when in 320 mode)
constexpr uint32_t _COLORBYTESOFFSET = 1 + 32;	// the color bytes are offset every line by 33 (after SCBs and palette)

class A2VideoManager
{
public:

	//////////////////////////////////////////////////////////////////////////
	// SDHR state structs
	//////////////////////////////////////////////////////////////////////////

		// NOTE:	Anything labled "id" is an internal identifier by the GPU
		//			Anything labled "index" is an actual array or vector index used by the code

		// An image asset is a texture with its metadata (width, height)
		// The actual texture data is in the GPU memory
	struct ImageAsset {
		void AssignByFilename(A2VideoManager* owner, const char* filename);

		// image assets are full 32-bit bitmap files, uploaded from PNG
		uint32_t image_xcount = 0;	// width and height of asset in pixels
		uint32_t image_ycount = 0;
		GLuint tex_id = UINT_MAX;	// Texture ID on the GPU that holds the image data
	};

	// We'll create 2 BeamRenderVRAMs objects, for double buffering
	struct BeamRenderVRAMs {
		uint32_t id = 0;
		uint64_t frame_idx = 0;
		bool bWasRendered = false;
		A2Mode_e mode = A2Mode_e::NONE;
		uint8_t* vram_legacy = nullptr;
		uint8_t* vram_shr = nullptr;
		GLfloat* offset_buffer = nullptr;
	};

	//////////////////////////////////////////////////////////////////////////
	// Attributes
	//////////////////////////////////////////////////////////////////////////

	ImageAsset image_assets[8];
	std::unique_ptr<A2WindowBeam> windowsbeam[A2VIDEOBEAM_TOTAL_COUNT];	// beam racing GPU render

	uint64_t current_frame_idx = 0;
	uint64_t rendered_frame_idx = UINT64_MAX;

    bool bShouldReboot = false;             // When an Appletini reboot packet arrives
	uXY ScreenSize();

	bool bForceSHRWidth = false;			// forces the legacy to have the SHR width
	
	//////////////////////////////////////////////////////////////////////////
	// Methods
	//////////////////////////////////////////////////////////////////////////

	bool IsReady();		// true after full initialization
	void ToggleA2Video(bool value);

	// Methods for the single multipurpose beam racing shader
	void BeamIsAtPosition(uint32_t _x, uint32_t _y);

	void ForceBeamFullScreenRender();
	
	const uint32_t GetVRAMReadId() { return vrams_read->id; };
	const uint8_t* GetLegacyVRAMReadPtr() { return vrams_read->vram_legacy; };
	const uint8_t* GetSHRVRAMReadPtr() { return vrams_read->vram_shr; };
	const GLfloat* GetOffsetBufferReadPtr() { return vrams_read->offset_buffer; };
	uint8_t* GetLegacyVRAMWritePtr() { return vrams_write->vram_legacy; };
	uint8_t* GetSHRVRAMWritePtr() { return vrams_write->vram_shr; };
	GLfloat* GetOffsetBufferWritePtr() { return vrams_write->offset_buffer; };
	GLuint GetOutputTextureId();	// merged output
	void ActivateBeam();	// The apple 2 is rendering!
	void DeactivateBeam();	// We don't have a connection to the Apple 2!
	GLuint Render();		// render whatever mode is active (enabled windows)

	inline uint32_t GetVramWidthLegacy() { return (40 + (2 * borders_w_cycles)); };	// in 4 bytes!
	inline uint32_t GetVramHeightLegacy() { return  (192 + (2 * borders_h_scanlines)); };
	inline uint32_t GetVramSizeLegacy() { return (GetVramWidthLegacy() * GetVramHeightLegacy() * 4); };	// in bytes

	inline uint32_t GetVramWidthSHR() { return (_COLORBYTESOFFSET + (2 * borders_w_cycles * 4) + 160); };	// in 4 bytes!
	inline uint32_t GetVramHeightSHR() { return  (200 + (2 * borders_h_scanlines)); };
	inline uint32_t GetVramSizeSHR() { return (GetVramWidthSHR() * GetVramHeightSHR() * 4); };	// in bytes

	// Changing borders reinitializes everything
	// Pass in a cycle for width (7 or 8 (SHR) lines per increment)
	// And a height (8 lines per increment)
	void SetBordersWithReinit(uint8_t width_cycles, uint8_t height_8s);
	uint32_t GetBordersWidthCycles() const { return borders_w_cycles; }
	uint32_t GetBordersHeightScanlines() const { return borders_h_scanlines; }

	// public singleton code
	static A2VideoManager* GetInstance()
	{
		if (NULL == s_instance)
			s_instance = new A2VideoManager();
		return s_instance;
	}
	~A2VideoManager();
    
	void Initialize();
	void ResetComputer();
private:
	static A2VideoManager* s_instance;
	A2VideoManager()
	{
		vrams_array = new BeamRenderVRAMs[2];
		Initialize();
	}
	void StartNextFrame();
	void SwitchToMergedMode(uint32_t scanline);
	void InitializeFullQuad();
	void PrepareOffsetTexture();

	//////////////////////////////////////////////////////////////////////////
	// Internal data
	//////////////////////////////////////////////////////////////////////////
	bool bIsReady = false;
	bool bA2VideoEnabled = true;			// Is standard Apple 2 video enabled?
	bool bShouldInitializeRender = true;	// Used to tell the render method to run initialization
    bool bIsRebooting = false;              // Rebooting semaphore
	bool bIsSwitchingToMergedMode = false;	// True when refreshing earlier scanlines for merged mode

	// beam render state variables
	bool bBeamIsActive = false;				// Is the beam active?
	BeamState_e beamState = BeamState_e::UNKNOWN;

	// Double-buffered vrams
	BeamRenderVRAMs* vrams_array;	// 2 buffers of legacy+shr vrams
	BeamRenderVRAMs* vrams_write;	// the write buffer
	BeamRenderVRAMs* vrams_read;	// the read buffer

	Shader shader_merge = Shader();

	VideoRegion_e current_region = VideoRegion_e::NTSC;
	uint32_t region_scanlines = (current_region == VideoRegion_e::NTSC ? SC_TOTAL_NTSC : SC_TOTAL_PAL);

	GLint last_viewport[4];		// Previous viewport used, so we don't clobber it
	GLuint merged_texture_id;	// the merged texture that merges both legacy+shr
	GLuint output_texture_id;	// the actual output texture (could be legacy/shr/merged)
	GLuint FBO_merged = UINT_MAX;		// the framebuffer object for the merge
	GLuint quadVAO = UINT_MAX;
	GLuint quadVBO = UINT_MAX;

	// OFFSET buffer texture. Holds one signed int for each scanline to tell the shader
	// how much to offset by x a line for the sine wobble of the merge
	// The offset is negative for 14->16MHz and positive for 16->14MHz
	// The offset curve is:
	//			negative_offset = 4 - 1.1205^(d+28)
	//			positive_offset = 4 - 1.1205^(-d+28)
	// where d is the distance in scanlines from the mode switch
	// Essentially the curve moves from +/- 16 pixels when d is 0 back down to 0 when d is 20
	bool offsetTextureExists = false;
	unsigned int OFFSETTEX = UINT_MAX;
	A2Mode_e merge_last_change_mode = A2Mode_e::NONE;
	uint32_t merge_last_change_y = UINT_MAX;

	// Those could be anywhere up to 6 or 7 cycles for horizontal borders
	// and a lot more for vertical borders. We just decided on a size
	// But SHR starts VBLANK just like legacy modes, at scanline 192. Hence
	// it has 8 less bottom border scanlines than legacy.
	uint32_t borders_w_cycles = 5;
	uint32_t borders_h_scanlines = 8 * 2;	// multiple of 8

	// The merged framebuffer width is going to be shr + border
	GLint fb_width = 0;
	GLint fb_height = 0;
	// The actual final output width and height
	GLint output_width = 0;
	GLint output_height = 0;
};
#endif // A2VIDEOMANAGER_H

