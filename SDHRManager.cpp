#include "SDHRManager.h"
#include <cstring>
#include <zlib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <algorithm>
#include "SDL.h"
#ifdef _DEBUGTIMINGS
#include <chrono>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "OpenGLHelper.h"

// below because "The declaration of a static data member in its class definition is not a definition"
SDHRManager* SDHRManager::s_instance;

static OpenGLHelper* oglHelper = OpenGLHelper::GetInstance();
// The standard default shader for the windows and their mosaics
static Shader defaultWindowShaderProgram = Shader();

//////////////////////////////////////////////////////////////////////////
// Commands structs
//////////////////////////////////////////////////////////////////////////

#pragma pack(push)
#pragma pack(1)

struct UploadDataCmd {
	uint16_t dest_block;
	uint16_t source_addr;
};

struct UploadDataFilenameCmd {
	uint8_t dest_addr_med;
	uint8_t dest_addr_high;
	uint8_t filename_length;
	uint8_t filename[];
};

struct DefineImageAssetCmd {
	uint8_t asset_index;
	uint16_t block_count;
};

struct DefineImageAssetFilenameCmd {
	uint8_t asset_index;
	uint8_t filename_length;
	uint8_t filename[];  // don't include the trailing null either in the data or counted in the filename_length
};

struct DefineTilesetCmd {
	uint8_t tileset_index;
	uint8_t asset_index;
	uint8_t num_entries;
	uint16_t xdim;			// xy dimension, in pixels, of tiles
	uint16_t ydim;
	uint16_t block_count;
};

struct DefineTilesetImmediateCmd {
	uint8_t tileset_index;
	uint8_t asset_index;
	uint8_t num_entries;
	uint8_t xdim;			// xy dimension, in pixels, of tiles
	uint8_t ydim;
	uint8_t data[];  // data is 4-byte records, 16-bit x and y offsets (scaled by x/ydim), from the given asset
};

struct DefineWindowCmd {
	uint8_t window_index;
	uint16_t screen_xcount;		// width in pixels of visible screen area of window
	uint16_t screen_ycount;
	uint16_t tile_xdim;			// xy dimension, in pixels, of tiles in the window.
	uint16_t tile_ydim;
	uint16_t tile_xcount;		// xy dimension, in tiles, of the tile array
	uint16_t tile_ycount;
};

struct UpdateWindowSetImmediateCmd {
	uint8_t window_index;
	uint16_t data_length;
};

struct UpdateWindowSetUploadCmd {
	uint8_t window_index;
	uint16_t block_count;
};

struct UpdateWindowShiftTilesCmd {
	uint8_t window_index;
	int8_t x_dir; // +1 shifts tiles right by 1, negative shifts tiles left by 1, zero no change
	int8_t y_dir; // +1 shifts tiles down by 1, negative shifts tiles up by 1, zero no change
};

struct UpdateWindowSetWindowPositionCmd {
	uint8_t window_index;
	int32_t screen_xbegin;
	int32_t screen_ybegin;
};

struct UpdateWindowAdjustWindowViewCommand {
	uint8_t window_index;
	int32_t tile_xbegin;
	int32_t tile_ybegin;
};

struct UpdateWindowEnableCmd {
	uint8_t window_index;
	uint8_t enabled;
};

struct UpdateWindowSetWindowSizeCommand {
	uint8_t window_index;
	uint32_t screen_xcount;		// width in pixels of visible screen area of window
	uint32_t screen_ycount;
};

struct ChangeResolutionCmd {
	uint32_t width;
	uint32_t height;
};

#pragma pack(pop)

//////////////////////////////////////////////////////////////////////////
// Image Asset Methods
//////////////////////////////////////////////////////////////////////////

// NOTE:	Both the below image asset methods use OpenGL 
//			so they _must_ be called from the main thread
void SDHRManager::ImageAsset::AssignByFilename(SDHRManager* owner, const char* filename) {
	std::cerr << "AssignByFilename" << std::endl;
	int width;
	int height;
	int channels;
	unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);
	if (data == NULL) {
		owner->CommandError(stbi_failure_reason());
		owner->error_flag = true;
		return;
	}
	if (tex_id != UINT_MAX)
	{
		oglHelper->load_texture(data, width, height, channels, tex_id);
		stbi_image_free(data);
	}
	else {
		std::cerr << "ERROR: Could not bind texture, all slots filled!" << '\n';
		return;
	}
	GLenum glerr;
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "ImageAsset::AssignByMemory error: " << glerr << std::endl;
	}
	image_xcount = width;
	image_ycount = height;
}

void SDHRManager::ImageAsset::AssignByMemory(SDHRManager* owner, const uint8_t* buffer, int size) {
	std::cerr << "AssignByMemory" << std::endl;
	int width = 0;
	int height = 0;
	int channels = 0;
	int fd = open("dumpfile", O_CREAT | O_WRONLY);
	std::cerr << "open dumpfile " << fd << std::endl;
	write(fd,buffer,size);
	close(fd);
	unsigned char* data = stbi_load_from_memory(buffer, size, &width, &height, &channels, 4);
	if (data == NULL) {
		owner->CommandError(stbi_failure_reason());
		return;
	}
	if (tex_id != UINT_MAX)
	{
		oglHelper->load_texture(data, width, height, channels, tex_id);
		stbi_image_free(data);
	} else {
		std::cerr << "ERROR: Could not bind texture, all slots filled!" << '\n';
		return;
	}
	GLenum glerr;
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "ImageAsset::AssignByMemory error: " << glerr << std::endl;
	}
	image_xcount = width;
	image_ycount = height;
}

//////////////////////////////////////////////////////////////////////////
// Static Methods
//////////////////////////////////////////////////////////////////////////

int upload_inflate(const char* source, uint32_t size, std::ostream& dest) {
	static const int CHUNK = 16384;
	int ret;
	unsigned have;
	z_stream strm;
	unsigned char* in; //[CHUNK] ;
	unsigned char* out; //[CHUNK] ;
	in = (unsigned char*)malloc(CHUNK);
	if (in == NULL)
		return Z_MEM_ERROR;
	out = (unsigned char*)malloc(CHUNK);
	if (out == NULL)
		return Z_MEM_ERROR;

	/* allocate inflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	ret = inflateInit2(&strm, (15 + 32));
	if (ret != Z_OK)
		return ret;

	/* decompress until deflate stream ends or end of file */
	uint32_t bytes_read = 0;
	while (bytes_read < size) {
		uint32_t bytes_to_read = std::min((uint32_t)CHUNK, size - bytes_read);
		memcpy(in, source + bytes_read, bytes_to_read);
		bytes_read += bytes_to_read;
		strm.avail_in = (unsigned int)bytes_to_read;
		if (strm.avail_in == 0)
			break;
		strm.next_in = in;

		/* run inflate() on input until output buffer not full */
		do {
			strm.avail_out = CHUNK;
			strm.next_out = out;
			ret = inflate(&strm, Z_NO_FLUSH);
			assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
			switch (ret) {
			case Z_NEED_DICT:
				ret = Z_DATA_ERROR;     /* and fall through */
			case Z_DATA_ERROR:
			case Z_MEM_ERROR:
				(void)inflateEnd(&strm);
				return ret;
			}
			have = CHUNK - strm.avail_out;
			dest.write((char*)out, have);
		} while (strm.avail_out == 0);

		/* done when inflate() says it's done */
	} while (ret != Z_STREAM_END);

	/* clean up and return */
	(void)inflateEnd(&strm);
	free(in);
	free(out);
	return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

//////////////////////////////////////////////////////////////////////////
// Methods
//////////////////////////////////////////////////////////////////////////

void SDHRManager::Initialize()
{
	bSDHREnabled = false;
	error_flag = false;
	memset(error_str, 0, sizeof(error_str));
	*image_assets = {};
	*tileset_records = {};

	for (size_t i = 0; i < (sizeof(windows)/sizeof(SDHRWindow)); i++)
	{
		// Set the z index of each window and keep track of them
		windows[i].Set_index(i);
	}

	command_buffer.clear();
	command_buffer.reserve(64 * 1024);

	// Initialize the Apple 2 memory duplicate
	// Whenever memory is written from the Apple2
	// in the main bank between $200 and $BFFF it will
	// be sent through the socket and this buffer will be updated
	memset(a2mem, 0, 0xc000);

	// tell the next Render() call to run initialization routines
	// Assign to the GPU the default pink image to all 16 image assets
	// because the shaders expect 16 textures
	for (size_t i = 0; i < _SDHR_MAX_TEXTURES; i++)
	{
		image_assets[i].tex_id = oglHelper->get_texture_id_at_slot(i);
	}
	if (!defaultWindowShaderProgram.isReady)
		defaultWindowShaderProgram.build(_SHADER_SDHR_VERTEX_DEFAULT, _SHADER_SDHR_FRAGMENT_DEFAULT);
	bShouldInitializeRender = true;
	threadState = THREADCOMM_e::IDLE;
	dataState = DATASTATE_e::NODATA;
}

SDHRManager::~SDHRManager()
{
	for (uint16_t i = 0; i < 256; ++i) {
		if (tileset_records[i].tile_data) {
			free(tileset_records[i].tile_data);
		}
	}
	delete[] a2mem;
	free(uploaded_data_region);
}

void SDHRManager::AddPacketDataToBuffer(uint8_t data)
{
	command_buffer.push_back(data);
}

void SDHRManager::ClearBuffer()
{
	command_buffer.clear();
}

void SDHRManager::CommandError(const char* err) {
	strcpy(error_str, err);
	error_flag = true;
	std::cerr << "Command Error: " << error_str << std::endl;
}

bool SDHRManager::CheckCommandLength(uint8_t* p, uint8_t* e, size_t sz) {
	size_t command_length = e - p;
	if (command_length < sz) {
		CommandError("Insufficient buffer space");
		return false;
	}
	return true;
}

// Return a pointer to the shadowed apple 2 memory
uint8_t* SDHRManager::GetApple2MemPtr()
{
	return a2mem;
}

uint8_t* SDHRManager::GetUploadRegionPtr()
{ 
	return uploaded_data_region;
};


// Render all window meshes and whatever else SDHR related
void SDHRManager::Render()
{
	GLenum glerr;
	auto oglh = OpenGLHelper::GetInstance();

	oglh->setup_sdhr_render();

	if (bDidChangeResolution) {
		oglh->rescale_framebuffer(rendererOutputWidth, rendererOutputHeight);
	}

	defaultWindowShaderProgram.use();
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL glUseProgram error: " << glerr << std::endl;
	}

	// Initialization routine runs only once on init (or re-init)
	// We do that here because we know the framebuffer is bound, and everything
	// for drawing the SDHR stuff is active
	if (bShouldInitializeRender) {
		bShouldInitializeRender = false;

		// We're going to set the active textures to _SDHR_START_TEXTURES, leaving textures GL_TEXTURE0 (output texture)
		// and GL_TEXTURE1 (mosaic data buffer) alone
		for (size_t i = 0; i < _SDHR_MAX_TEXTURES; i++) {
			glActiveTexture(_SDHR_START_TEXTURES + i);	// AssignByFilename() will bind to the active texture slot
			// the default tex0 and tex4..16 are the same, but the others are unique for better testing
			if (i == 1)
				image_assets[i].AssignByFilename(this, "Texture_Default1.png");
			else if (i == 2)
				image_assets[i].AssignByFilename(this, "Texture_Default2.png");
			else if (i == 3)
				image_assets[i].AssignByFilename(this, "Texture_Default3.png");
			else
				image_assets[i].AssignByFilename(this, "Texture_Default.png");
			texSamplers[i] = (_SDHR_START_TEXTURES - GL_TEXTURE0) + i;
			if ((glerr = glGetError()) != GL_NO_ERROR) {
				std::cerr << "OpenGL AssignByFilename error: " << i << " - " << glerr << std::endl;
			}
		}
		glActiveTexture(GL_TEXTURE0);
	}

	if (this->dataState == DATASTATE_e::COMMAND_READY)
	{
		// Check to see if we need to upload data to the GPU
		this->threadState = THREADCOMM_e::MAIN_LOCK;
		while (!fifo_upload_image_data.empty()) {
			auto _uidata = fifo_upload_image_data.front();
			glActiveTexture(_SDHR_START_TEXTURES + _uidata.asset_index);
			image_assets[_uidata.asset_index].AssignByMemory(this, uploaded_data_region + _uidata.upload_start_addr, _uidata.upload_data_size);
			if (error_flag) {
				std::cerr << "AssignByMemory failed!" << std::endl;
			}
			fifo_upload_image_data.pop();
#ifdef _DEBUG
			std::cout << "AssignByMemory: " << _uidata.upload_data_size << " for index: " << (uint32_t)_uidata.asset_index << std::endl;
#endif
			glActiveTexture(GL_TEXTURE0);
		}
		GLenum glerr;
		if ((glerr = glGetError()) != GL_NO_ERROR) {
			std::cerr << "OpenGL error BEFORE window update: " << glerr << std::endl;
		}
		// Update windows and meshes
		for (auto& _w: this->windows) {
			_w.Update();
		}
		if ((glerr = glGetError()) != GL_NO_ERROR) {
			std::cerr << "OpenGL render SDHRManager error: " << glerr << std::endl;
		}
		this->dataState = DATASTATE_e::NODATA;
		this->threadState = THREADCOMM_e::IDLE;
	}


	// Assign the list of all the textures to the shader's "tilesTexture" uniform
	auto texUniformId = glGetUniformLocation(defaultWindowShaderProgram.ID, "tilesTexture");
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL glGetUniformLocation error: " << glerr << std::endl;
	}
	glUniform1iv(texUniformId, _SDHR_MAX_TEXTURES, &texSamplers[0]);
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL glUniform1iv error: " << glerr << std::endl;
	}

	// Assign the sdhr global (to all windows) uniforms
	defaultWindowShaderProgram.setBool("iDebugNoTextures", bDebugNoTextures);
	defaultWindowShaderProgram.setInt("ticks", SDL_GetTicks());

	// bUsePerspective toggle:
	// Test using a prespective so we can zoom back and forth easily
	// perspective uses (fov, aspect, near, far)
	// Default FOV is 45 degrees

	if (bUsePerspective && (bDidChangeResolution || (!bIsUsingPerspective)))
	{
		camera.Position.x = rendererOutputWidth / 2.f;
		camera.Position.y = rendererOutputHeight / 2.f;
		camera.Position.z = glm::cos(glm::radians(ZOOM)) * rendererOutputWidth;
		mat_proj = glm::perspective<float>(glm::radians(this->camera.Zoom), (float)rendererOutputWidth / rendererOutputHeight, 0, 256);
		bIsUsingPerspective = bUsePerspective;
	}
	else if ((!bUsePerspective) && (bDidChangeResolution || bIsUsingPerspective))
	{
		camera.Position.x = rendererOutputWidth / 2.f;
		camera.Position.y = rendererOutputHeight / 2.f;
		camera.Position.z = _SDHR_MAX_WINDOWS;
		mat_proj = glm::ortho<float>(-rendererOutputWidth/2, rendererOutputWidth/2, -rendererOutputHeight/2, rendererOutputHeight/2, 0, 256);
		bIsUsingPerspective = bUsePerspective;
	}

	// And always update the projection when in perspective due to the zoom state
	if (bUsePerspective)
		mat_proj = glm::perspective<float>(glm::radians(this->camera.Zoom), (float)rendererOutputWidth / rendererOutputHeight, 0, 256);

	// Render the windows (i.e. the meshes with the windows stencils)
	for (auto& _w: this->windows) {
		_w.Render(this->camera.GetViewMatrix(), mat_proj);
	}
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL draw error: " << glerr << std::endl;
	}
	oglh->cleanup_sdhr_render();
	bDidChangeResolution = false;
}

// Define a tileset from the SDHR_CMD_DEFINE_TILESET commands
// The tileset data is kept in the CPU's memory while waiting for window data
// Once window data comes in, the tileset data is used to allocate the UVs to each vertex
void SDHRManager::DefineTileset(uint8_t tileset_index, uint16_t num_entries, uint16_t xdim, uint16_t ydim,
	uint8_t asset_index, uint8_t* offsets) {
	TilesetRecord* r = tileset_records + tileset_index;
	if (r->tile_data) {
		free(r->tile_data);
	}
	*r = {};
	r->asset_index = asset_index;
	r->xdim = xdim;
	r->ydim = ydim;
	r->num_entries = num_entries;
	r->tile_data = (TileTex*)malloc(sizeof(TileTex) * num_entries);
#ifdef DEBUG
	std::cout << "Allocating tile data size: " << sizeof(TileTex) * num_entries << " for index: " << (uint32_t)tileset_index << std::endl;
#endif

	uint8_t* offset_p = offsets;
	TileTex* tex_p = r->tile_data;
	for (uint32_t i = 0; i < num_entries; ++i) {
		uint32_t xoffset = *((uint16_t*)offset_p);
		offset_p += 2;
		uint32_t yoffset = *((uint16_t*)offset_p);
		offset_p += 2;
		tex_p->upos = xoffset * xdim;
		tex_p->vpos = yoffset * ydim;
		++tex_p;
	}
}

/**
 * Commands in the buffer look like:
 * First 2 bytes are the command length (excluding these bytes)
 * 3rd byte is the command id
 * Anything after that is the command's packed struct,
 * for example UpdateWindowEnableCmd.
 * So the buffer of UpdateWindowEnable will look like:
 * {03, 00, 13, 0, 1} to enable window 0
*/

bool SDHRManager::ProcessCommands(void)
{
	if (error_flag) {
		return false;
	}
	if (command_buffer.empty()) {
		//nothing to do
		return true;
	}
	uint8_t* begin = &command_buffer[0];
	uint8_t* end = begin + command_buffer.size();
	uint8_t* p = begin;

#ifdef DEBUG
	std::cerr << "Command buffer size: " << command_buffer.size() << std::endl;
#endif

	while (p < end) {
		// Header (2 bytes) giving the size in bytes of the command
		if (!CheckCommandLength(p, end, 2)) {
			std::cerr << "CheckCommandLength failed!" << std::endl;
			return false;
		}
		uint16_t message_length = *((uint16_t*)p);
		if (!CheckCommandLength(p, end, message_length)) return false;
		p += 2;
		// Command ID (1 byte)
		uint8_t _cmd = *p++;
		// Command data (variable)
		switch (_cmd) {
		case SDHR_CMD_UPLOAD_DATA: {
			if (!CheckCommandLength(p, end, sizeof(UploadDataCmd))) return false;
			UploadDataCmd* cmd = (UploadDataCmd*)p;
			uint32_t dest_offset = (uint32_t)cmd->dest_block * 512;
			uint32_t data_size = (uint32_t)512;
			if (!DataSizeCheck(dest_offset, data_size)) {
				std::cerr << "DataSizeCheck failed!" << std::endl;
				return false;
			}
			// Check if there's a pending image upload 
			// Wait until it's done
			//while (!fifo_upload_image_data.empty()) {};
			while (this->dataState == DATASTATE_e::COMMAND_READY) {}
			/*
			std::cout << std::hex << "Uploaded from: " << (uint32_t)(cmd->source_addr) 
				<< " To: " << (uint32_t)(uploaded_data_region + dest_offset)
				<< " Amount: " << std::dec << (uint32_t)data_size
				<< " Destination Block: " << (uint32_t)cmd->dest_block
				<< std::endl;
			*/
			memcpy(uploaded_data_region + dest_offset, a2mem + ((uint16_t)cmd->source_addr), data_size);
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPLOAD_DATA: Success: " << std::hex << data_size << std::endl;
#endif
		} break;
		case SDHR_CMD_DEFINE_IMAGE_ASSET: {
			if (!CheckCommandLength(p, end, sizeof(DefineImageAssetCmd))) return false;
			DefineImageAssetCmd* cmd = (DefineImageAssetCmd*)p;
			uint32_t upload_start_addr = 0;
			int upload_data_size = (int)cmd->block_count * 512;

			auto _uidata = UploadImageData();
			_uidata.asset_index = cmd->asset_index;
			_uidata.upload_start_addr = upload_start_addr;
			_uidata.upload_data_size = upload_data_size;
			fifo_upload_image_data.push(_uidata);
#ifdef DEBUG
			std::cout << "SDHR_CMD_DEFINE_IMAGE_ASSET: Success:" 
				<< std::dec << r->image_xcount << " x " << r->image_ycount << std::endl;
#endif
		} break;
		case SDHR_CMD_DEFINE_IMAGE_ASSET_FILENAME: {
			std::cerr << "SDHR_CMD_DEFINE_IMAGE_ASSET_FILENAME: Not Implemented." << std::endl;
			// NOT IMPLEMENTED
			// NOTE: Implementation would have to make sure it's the main thread that loads the image asset
		} break;
		case SDHR_CMD_UPLOAD_DATA_FILENAME: {
			std::cerr << "SDHR_CMD_UPLOAD_DATA_FILENAME: Not Implemented." << std::endl;
			// NOT IMPLEMENTED
		} break;
		case SDHR_CMD_DEFINE_TILESET: {
			if (!CheckCommandLength(p, end, sizeof(DefineTilesetCmd))) return false;
			DefineTilesetCmd* cmd = (DefineTilesetCmd*)p;
			uint16_t num_entries = cmd->num_entries;
			if (num_entries == 0) {
				num_entries = 256;
			}
			uint32_t required_data_size = num_entries * 4;
			if (cmd->block_count * 512 < required_data_size) {
				CommandError("Insufficient data space for tileset");
			}
			DefineTileset(cmd->tileset_index, num_entries, cmd->xdim, cmd->ydim, cmd->asset_index, uploaded_data_region);
#ifdef DEBUG
			std::cout << "SDHR_CMD_DEFINE_TILESET: Success! "
				<< std::dec << (uint32_t)cmd->tileset_index << ';'<< (uint32_t)num_entries << std::endl;
#endif
		} break;
		case SDHR_CMD_DEFINE_TILESET_IMMEDIATE: {
			if (!CheckCommandLength(p, end, sizeof(DefineTilesetImmediateCmd))) return false;
			DefineTilesetImmediateCmd* cmd = (DefineTilesetImmediateCmd*)p;
			uint16_t num_entries = cmd->num_entries;
			if (num_entries == 0) {
				num_entries = 256;
			}
			uint32_t load_data_size;
			load_data_size = (uint32_t)num_entries * 4;
			if (message_length != sizeof(DefineTilesetImmediateCmd) + load_data_size + 3) {	// 3 is cmd_len and cmd_id
				CommandError("DefineTilesetImmediate data size mismatch");
				return false;
			}
			DefineTileset(cmd->tileset_index, num_entries, cmd->xdim, cmd->ydim, cmd->asset_index, cmd->data);
#ifdef DEBUG
			std::cout << "SDHR_CMD_DEFINE_TILESET_IMMEDIATE: Success! " 
				<< std::dec << (uint32_t)cmd->tileset_index << ';' << (uint32_t)num_entries << std::endl;
#endif
		} break;
		case SDHR_CMD_DEFINE_WINDOW: {
			if (!CheckCommandLength(p, end, sizeof(DefineWindowCmd))) return false;
			DefineWindowCmd* cmd = (DefineWindowCmd*)p;
			SDHRWindow* r = windows + cmd->window_index;
			auto sc = r->Get_screen_count();
			if (sc.x > (uint32_t)rendererOutputWidth) {
				CommandError("Window exceeds max x resolution");
				return false;
			}
			if (sc.y > (uint32_t)rendererOutputHeight) {
				CommandError("Window exceeds max y resolution");
				return false;
			}
			r->Define(
				uXY({ cmd->screen_xcount, cmd->screen_ycount }),
				uXY({ cmd->tile_xdim, cmd->tile_ydim }),
				uXY({ cmd->tile_xcount, cmd->tile_ycount }),
				&defaultWindowShaderProgram
			);

#ifdef DEBUG
			std::cout << "SDHR_CMD_DEFINE_WINDOW: Success! " 
				<< std::dec << cmd->window_index << ';' << (uint32_t)cmd->tile_xcount << ';' << (uint32_t)cmd->tile_ycount << std::endl;
#endif
		} break;
		case SDHR_CMD_UPDATE_WINDOW_SET_IMMEDIATE: {
			size_t cmd_sz = sizeof(UpdateWindowSetImmediateCmd);
			if (!CheckCommandLength(p, end, cmd_sz)) return false;
			UpdateWindowSetImmediateCmd* cmd = (UpdateWindowSetImmediateCmd*)p;
			SDHRWindow* r = windows + cmd->window_index;

			// full tile specification: tileset and index
			auto wintilect = r->Get_tile_count();
			uint32_t required_data_size = wintilect.x * wintilect.y * 2;
			if (required_data_size != cmd->data_length) {
				CommandError("UpdateWindowSetImmediate data size mismatch");
				return false;
			}
			if (!CheckCommandLength(p, end, cmd_sz + cmd->data_length)) return false;
			// Allocate to each vertex:
			//  u, v coordinates of the texture (based on the tileset's tile index)
			//  textureId of the image asset used in the tileset
			uint8_t* sp = p + cmd_sz;
			auto mesh = r->mesh;
			for (uint32_t i = 0; i < cmd->data_length / 2; ++i) {
				uint8_t tileset_index = sp[i * 2];
				uint8_t tile_index = sp[i * 2 + 1];
				const TilesetRecord tr = tileset_records[tileset_index];
				if (tr.xdim != r->Get_tile_dim().x ||
					tr.ydim != r->Get_tile_dim().y ||
					tr.num_entries <= tile_index) {
					CommandError("invalid tile specification");
					return false;
				}
				mesh->UpdateMosaicUV(
					i,
					tr.tile_data[tile_index].upos,
					tr.tile_data[tile_index].vpos,
					tr.asset_index);
			}
			p += cmd->data_length;
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_SET_IMMEDIATE: Success!" << std::endl;
#endif
		} break;
		case SDHR_CMD_UPDATE_WINDOW_SET_UPLOAD: {
			if (!CheckCommandLength(p, end, sizeof(UpdateWindowSetUploadCmd))) return false;
			UpdateWindowSetUploadCmd* cmd = (UpdateWindowSetUploadCmd*)p;
			SDHRWindow* r = windows + cmd->window_index;
			// full tile specification: tileset and index
			uint32_t data_size = (uint32_t)cmd->block_count * 512;
			std::stringstream ss;
			upload_inflate((const char*)uploaded_data_region, data_size, ss);
			std::string s = ss.str();

			auto wintilect = r->Get_tile_count();
			if (s.length() != wintilect.x * wintilect.y * 2) {
				CommandError("UploadWindowSetUpload data insufficient to define window tiles");
			}
			//  Allocate to each vertex:
			//  u, v coordinates of the texture (based on the tileset's tile index)
			//  textureId of the image asset used in the tileset
			//  NOTE: U/V has its 0,0 origin at the top left. OpenGL is bottom left
			uint8_t* sp = (uint8_t*)s.c_str();
			auto mesh = r->mesh;
			for (uint32_t tile_y = 0; tile_y < wintilect.y; ++tile_y) {
				// uint32_t line_offset = (uint32_t)tile_y * r->tile_xcount;
				for (uint32_t tile_x = 0; tile_x < wintilect.x; ++tile_x) {
					uint8_t tileset_index = *sp++;
					uint8_t tile_index = *sp++;
					const TilesetRecord tr = tileset_records[tileset_index];
					if (tr.xdim != r->Get_tile_dim().x ||
						tr.ydim != r->Get_tile_dim().y ||
						tr.num_entries <= tile_index) {
						CommandError("invalid tile specification");
						return false;
					}

					mesh->UpdateMosaicUV(
						tile_x, tile_y,
						tr.tile_data[tile_index].upos, tr.tile_data[tile_index].vpos,
						tr.asset_index);
				}
			}
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_SET_UPLOAD: Success!" << std::endl;
#endif
		} break;
/*
		case SDHR_CMD_UPDATE_WINDOW_SINGLE_TILESET: {
			if (!CheckCommandLength(p, end, sizeof(UpdateWindowSingleTilesetCmd))) return false;
			UpdateWindowSingleTilesetCmd* cmd = (UpdateWindowSingleTilesetCmd*)p;
			SDHRWindow* r = windows + cmd->window_index;
			auto wintilect = r->Get_tile_count();
			if ((uint32_t)cmd->tile_xbegin + cmd->tile_xcount > wintilect.x ||
				(uint32_t)cmd->tile_ybegin + cmd->tile_ycount > wintilect.y) {
				CommandError("tile update region exceeds tile dimensions");
				return false;
			}
			// partial tile specification: index and palette, single tileset
			uint32_t data_size = (uint32_t)cmd->tile_xcount * cmd->tile_ycount;
			if (data_size + sizeof(UpdateWindowSingleTilesetCmd) != message_length) {
				CommandError("UpdateWindowSingleTileset data size mismatch");
				return false;
			}
			uint8_t* dp = cmd->data;
			for (uint32_t tile_y = 0; tile_y < cmd->tile_ycount; ++tile_y) {
				uint32_t line_offset = (cmd->tile_ybegin + tile_y) * wintilect.x + cmd->tile_xbegin;
				for (uint32_t tile_x = 0; tile_x < cmd->tile_xcount; ++tile_x) {
					uint8_t tile_index = *dp++;
					if (tileset_records[cmd->tileset_index].xdim != r->Get_tile_dim().x ||
						tileset_records[cmd->tileset_index].ydim != r->Get_tile_dim().y ||
						tileset_records[cmd->tileset_index].num_entries <= tile_index) {
						CommandError("invalid tile specification");
						return false;
					}
					r->tileset_indexes[line_offset + tile_x] = cmd->tileset_index;
					r->tile_indexes[line_offset + tile_x] = tile_index;
				}
			}
			std::cout << "SDHR_CMD_UPDATE_WINDOW_SINGLE_TILESET: Success!" << std::endl;
		} break;
*/
		case SDHR_CMD_UPDATE_WINDOW_SHIFT_TILES: {
			if (!CheckCommandLength(p, end, sizeof(UpdateWindowShiftTilesCmd))) return false;
			UpdateWindowShiftTilesCmd* cmd = (UpdateWindowShiftTilesCmd*)p;
			SDHRWindow* r = windows + cmd->window_index;
			if (cmd->x_dir < -1 || cmd->x_dir > 1 || cmd->y_dir < -1 || cmd->y_dir > 1) {
				CommandError("invalid tile shift");
				return false;
			}
			r->ShiftTiles(iXY({ cmd->x_dir, cmd->y_dir }));

#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_SHIFT_TILES: Success! " 
				<< (uint32_t)cmd->window_index << std::dec << ';' << (uint32_t)cmd->x_dir << ';' << (uint32_t)cmd->y_dir << std::endl;
#endif
		} break;
		case SDHR_CMD_UPDATE_WINDOW_SET_WINDOW_POSITION: {
			if (!CheckCommandLength(p, end, sizeof(UpdateWindowSetWindowPositionCmd))) return false;
			UpdateWindowSetWindowPositionCmd* cmd = (UpdateWindowSetWindowPositionCmd*)p;
			SDHRWindow* r = windows + cmd->window_index;
			r->SetPosition(iXY({ cmd->screen_xbegin, cmd->screen_ybegin }));
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_SET_WINDOW_POSITION: Success! "
				<< (uint32_t)cmd->window_index << std::dec << ';' << (uint32_t)cmd->screen_xbegin << ';' << (uint32_t)cmd->screen_ybegin << std::endl;
#endif
		} break;
		case SDHR_CMD_UPDATE_WINDOW_ADJUST_WINDOW_VIEW: {
			if (!CheckCommandLength(p, end, sizeof(UpdateWindowAdjustWindowViewCommand))) return false;
			UpdateWindowAdjustWindowViewCommand* cmd = (UpdateWindowAdjustWindowViewCommand*)p;
			SDHRWindow* r = windows + cmd->window_index;
			r->AdjustView(iXY({ cmd->tile_xbegin, cmd->tile_ybegin }));
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_ADJUST_WINDOW_VIEW: Success! "
				<< (uint32_t)cmd->window_index << std::dec << ';' << (uint32_t)cmd->tile_xbegin << ';' << (uint32_t)cmd->tile_ybegin << std::endl;
#endif
		} break;
		case SDHR_CMD_UPDATE_WINDOW_SET_SIZE: {
			if (!CheckCommandLength(p, end, sizeof(UpdateWindowSetWindowSizeCommand))) return false;
			UpdateWindowSetWindowSizeCommand* cmd = (UpdateWindowSetWindowSizeCommand*)p;
			SDHRWindow* r = windows + cmd->window_index;
			r->SetSize(uXY({ cmd->screen_xcount, cmd->screen_ycount }));
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_SET_SIZE: Success! "
				<< (uint32_t)cmd->window_index << std::dec << ';' << (uint32_t)cmd->screen_xcount << ';' << (uint32_t)cmd->screen_ycount << std::endl;
#endif
		} break;
		case SDHR_CMD_UPDATE_WINDOW_ENABLE: {
			if (!CheckCommandLength(p, end, sizeof(UpdateWindowEnableCmd))) return false;
			UpdateWindowEnableCmd* cmd = (UpdateWindowEnableCmd*)p;
			SDHRWindow* r = windows + cmd->window_index;
			if (r->IsEmpty()) {
				CommandError("cannot enable empty window");
				return false;
			}
			r->enabled = cmd->enabled;
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_ENABLE: Success! "
				<< std::dec << (uint32_t)cmd->window_index << std::endl;
#endif
		} break;
		case SDHR_CMD_CHANGE_RESOLUTION: {
			if (!CheckCommandLength(p, end, sizeof(ChangeResolutionCmd))) return false;
			ChangeResolutionCmd* cmd = (ChangeResolutionCmd*)p;
			if ((rendererOutputWidth != (int)cmd->width) || (rendererOutputHeight != (int)cmd->height))
			{
				rendererOutputWidth = (int)cmd->width;
				rendererOutputHeight = (int)cmd->height;
				bDidChangeResolution = true;		// Get the render method to update the resolution
			}
		} break;
		default:
			CommandError("unrecognized command");
			return false;
		}
		p += message_length - 3;
	}
	return true;
}
