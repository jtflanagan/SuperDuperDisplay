#include "SDHRManager.h"
#include <cstring>
#include <zlib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <algorithm>
#ifdef _DEBUGTIMINGS
#include <chrono>
#endif

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
	uint8_t num_entries;
	uint8_t xdim;			// xy dimension, in pixels, of tiles
	uint8_t ydim;
	uint8_t asset_index;
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

#pragma pack(pop)

//////////////////////////////////////////////////////////////////////////
// Image Asset Methods
//////////////////////////////////////////////////////////////////////////

// NOTE:	Both the below image asset methods use OpenGL 
//			so they _must_ be called from the main thread
void SDHRManager::ImageAsset::AssignByFilename(SDHRManager* owner, const char* filename) {
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
	image_xcount = width;
	image_ycount = height;
}

void SDHRManager::ImageAsset::AssignByMemory(SDHRManager* owner, const uint8_t* buffer, int size) {
	int width;
	int height;
	int channels;
	unsigned char* data = stbi_load_from_memory(buffer, size, &width, &height, &channels, 4);
	if (data == NULL) {
		owner->CommandError(stbi_failure_reason());
		owner->error_flag = true;
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
	image_xcount = width;
	image_ycount = height;
}

//////////////////////////////////////////////////////////////////////////
// Static Methods
//////////////////////////////////////////////////////////////////////////

int upload_inflate(const char* source, uint64_t size, std::ostream& dest) {
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
	uint64_t bytes_read = 0;
	while (bytes_read < size) {
		uint64_t bytes_to_read = std::min((uint64_t)CHUNK, size - bytes_read);
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
	memset(uploaded_data_region, 0, sizeof(uploaded_data_region));
	*image_assets = {};
	*tileset_records = {};
	*windows = {};

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
		defaultWindowShaderProgram.build("shaders/sdhr_window_tr.vert", "shaders/sdhr_window_tr.frag");
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
		if (windows[i].mesh) {
			delete windows[i].mesh;
		}
	}
	delete[] a2mem;
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
	strcpy_s(error_str, err);
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

// Render all window meshes and whatever else SDHR related
void SDHRManager::Render()
{
	GLenum glerr;
	auto oglh = OpenGLHelper::GetInstance();
	oglh->setup_sdhr_render();

	defaultWindowShaderProgram.use();
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL glUseProgram error: " << glerr << std::endl;
	}

	// Initialization routine runs only once on init (or re-init)
	// We do that here because we know the framebuffer is bound, and everything
	// for drawing the SDHR stuff is active
	// We're going to set the active textures to 1-15, leaving texture GL_TEXTURE0 alone
	if (bShouldInitializeRender) {
		bShouldInitializeRender = false;
		for (size_t i = 0; i < _SDHR_MAX_TEXTURES; i++) {
			glActiveTexture(GL_TEXTURE1 + i);	// AssignByFilename() will bind to the active texture slot
			if (i == 1)
				image_assets[i].AssignByFilename(this, "Texture_Default1.png");
			else if (i == 2)
				image_assets[i].AssignByFilename(this, "Texture_Default2.png");
			else if (i == 3)
				image_assets[i].AssignByFilename(this, "Texture_Default3.png");
			else
				image_assets[i].AssignByFilename(this, "Texture_Default.png");
			texIds[i] = image_assets[i].tex_id;
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
			glActiveTexture(GL_TEXTURE1 + _uidata.asset_index);
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
		// Update meshes
		for each (auto & _w in this->windows) {
			if (_w.enabled) {
				if (_w.mesh) {
					_w.mesh->updateMesh();
				}
			}
		}
		this->dataState = DATASTATE_e::NODATA;
		this->threadState = THREADCOMM_e::IDLE;
		if ((glerr = glGetError()) != GL_NO_ERROR) {
			std::cerr << "OpenGL updateMesh error: " << glerr << std::endl;
		}
	}


	// Assign the list of all the textures to the shader's "tilesTexture" uniform
	auto texUniformId = glGetUniformLocation(defaultWindowShaderProgram.ID, "tilesTexture");
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL glGetUniformLocation error: " << glerr << std::endl;
	}
	glUniform1iv(texUniformId, _SDHR_MAX_TEXTURES, &texIds[0]);
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL glUniform1iv error: " << glerr << std::endl;
	}

	defaultWindowShaderProgram.setBool("bDebugTextures", bDebugTextures);

	// TEST
	// Using a prespective so I can zoom back and forth easily
	// perspective uses (fov, aspect, near, far)
	// If the camera is using a perspective projection matrix, specify the z starting distance
	// Default FOV is 45 degrees

	if (bUsePerspective && (!bIsUsingPerspective))
	{
		camera.Position.z = glm::cos(glm::radians(ZOOM)) * _SDHR_WIDTH;
		mat_proj = glm::perspective<float>(glm::radians(this->camera.Zoom), (float)_SDHR_WIDTH / _SDHR_HEIGHT, 0, 256);
		bIsUsingPerspective = bUsePerspective;
	}
	else if ((!bUsePerspective) && bIsUsingPerspective)
	{
		camera.Position.z = 10.f;
		mat_proj = glm::ortho<float>(-_SDHR_WIDTH/2, _SDHR_WIDTH/2, -_SDHR_HEIGHT/2, _SDHR_HEIGHT/2, 0, 256);
		bIsUsingPerspective = bUsePerspective;
	}


	// Render the windows (i.e. the meshes with the windows stencils)
	for each (auto& _w in this->windows) {
		if (_w.enabled) {
			if (_w.mesh) {
				_w.mesh->Draw(this->camera.GetViewMatrix(), mat_proj);
			}
		}
	}
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL draw error: " << glerr << std::endl;
	}
	oglh->cleanup_sdhr_render();
}

void SDHRManager::RenderTest()
{
	GLenum glerr;
	auto oglh = OpenGLHelper::GetInstance();
	oglh->setup_sdhr_render();

	defaultWindowShaderProgram.use();
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL glUseProgram error: " << glerr << std::endl;
	}

	// Initialization routine runs only once on init (or re-init)
	// We do that here because we know the framebuffer is bound, and everything
	// for drawing the SDHR stuff is active
	// We're going to set the active textures to 1-15, leaving texture GL_TEXTURE0 alone
	if (bShouldInitializeRender) {
		bShouldInitializeRender = false;
		for (size_t i = 0; i < _SDHR_MAX_TEXTURES; i++) {
			glActiveTexture(GL_TEXTURE1 + i);	// AssignByFilename() will bind to the active texture slot
			if (i == 1)
				image_assets[i].AssignByFilename(this, "Texture_Default1.png");
			else if (i == 2)
				image_assets[i].AssignByFilename(this, "Texture_Default2.png");
			else if (i == 3)
				image_assets[i].AssignByFilename(this, "Texture_Default3.png");
			else
				image_assets[i].AssignByFilename(this, "Texture_Default.png");
			texIds[i] = image_assets[i].tex_id;
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
			glActiveTexture(GL_TEXTURE1 + _uidata.asset_index);
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
		this->dataState = DATASTATE_e::NODATA;
		this->threadState = THREADCOMM_e::IDLE;
		if ((glerr = glGetError()) != GL_NO_ERROR) {
			std::cerr << "OpenGL updateMesh error: " << glerr << std::endl;
		}
	}

	// Assign the list of all the textures to the shader's "tilesTexture" uniform
	auto texUniformId = glGetUniformLocation(defaultWindowShaderProgram.ID, "tilesTexture");
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL glGetUniformLocation error: " << glerr << std::endl;
	}
	glUniform1iv(texUniformId, _SDHR_MAX_TEXTURES, &texIds[0]);
	if ((glerr = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL glUniform1iv error: " << glerr << std::endl;
	}

	defaultWindowShaderProgram.setBool("bDebugTextures", bDebugTextures);

	GLuint VertexArrayID;
	glGenVertexArrays(1, &VertexArrayID);
	glBindVertexArray(VertexArrayID);

	static const GLfloat g_vertex_buffer_data[] = {
			-0.5f, -0.5f, 0.0f,	9*0.0625f + 0.03125f,	0.f		,// 0.f,		//bl
			0.5f, 0.5f, 0.0f,	9*0.0625f + 0.0625f,	0.0625f	,// 0.f,		//tr
			-0.5f, 0.5f, 0.0f,	9*0.0625f + 0.03125f,	0.0625f	,// 0.f,		//tl
			0.5f, -0.5f, 0.0f,	9*0.0625f + 0.0625f,	0.f		,// 1.f,		//br
			0.5f, 0.5f, 0.0f,	9*0.0625f + 0.0625f,	0.0625f	,// 1.f,		//tr
			-0.5f, -0.5f, 0.0f,	9*0.0625f + 0.03125f,	0.f		,// 1.f,		//bl
	};
	static const GLbyte g_texidx_buffer_data[] = {
		0,		//bl
		0,		//tr
		0,		//tl
		1,		//br
		1,		//tr
		1,		//bl
	};

	GLuint vertexbuffer;
	glGenBuffers(1, &vertexbuffer);
	glBindBuffer(GL_ARRAY_BUFFER, vertexbuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertex_buffer_data), g_vertex_buffer_data, GL_STATIC_DRAW);

	// set the vertex attribute pointers
	// vertex Positions: position 0, size 3
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 5, (void*)0);
	// vertex texture coords: position 1, size 2
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 5, (void*)(sizeof(GLfloat) * 3));

	// WARNING: Must use glVertexAttribIPointer with uints, otherwise the shader doesn't pick that up!
	GLuint texidbuffer;
	glGenBuffers(1, &texidbuffer);
	glBindBuffer(GL_ARRAY_BUFFER, texidbuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(g_texidx_buffer_data), g_texidx_buffer_data, GL_STATIC_DRAW);
	// vertex texture index: position 0, size 1
	glEnableVertexAttribArray(2);
	glVertexAttribIPointer(2, 1, GL_BYTE, GL_FALSE, (void*)0);

	// TEST
	// Using a prespective so I can zoom back and forth easily
	mat_proj = glm::mat4(1);
	defaultWindowShaderProgram.setMat4("transform", mat_proj);
	// mat_proj = glm::perspective<float>(glm::radians(this->camera.Zoom), 1, 0, 256);
	// defaultWindowShaderProgram.setMat4("transform", mat_proj * this->camera.GetViewMatrix() * glm::mat4(1));

	glDrawArrays(GL_TRIANGLES, 0, 6); // Starting from vertex 0; 6 vertices total -> 2 triangles
	glDisableVertexAttribArray(0);

	oglh->cleanup_sdhr_render();
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
	for (uint64_t i = 0; i < num_entries; ++i) {
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
			uint64_t dest_offset = (uint64_t)cmd->dest_block * 512;
			uint64_t data_size = (uint64_t)512;
			if (!DataSizeCheck(dest_offset, data_size)) {
				std::cerr << "DataSizeCheck failed!" << std::endl;
				return false;
			}
			// Check if there's a pending image upload 
			// Wait until it's done
			//while (!fifo_upload_image_data.empty()) {};
			while (this->dataState == DATASTATE_e::COMMAND_READY) {}
			/*
			std::cout << std::hex << "Uploaded from: " << (uint64_t)(cmd->source_addr) 
				<< " To: " << (uint64_t)(uploaded_data_region + dest_offset)
				<< " Amount: " << std::dec << (uint64_t)data_size
				<< " Destination Block: " << (uint64_t)cmd->dest_block
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
			uint64_t upload_start_addr = 0;
			int upload_data_size = (int)cmd->block_count * 512;

			ImageAsset* r = image_assets + cmd->asset_index;

			auto _uidata = UploadImageData();
			_uidata.asset_index = cmd->asset_index;
			_uidata.upload_start_addr = upload_start_addr;
			_uidata.upload_data_size = upload_data_size;
			fifo_upload_image_data.push(_uidata);
#ifdef DEBUG
			std::cout << "SDHR_CMD_DEFINE_IMAGE_ASSET: Success:" << r->image_xcount << " x " << r->image_ycount << std::endl;
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
			uint64_t required_data_size = num_entries * 4;
			if (cmd->block_count * 512 < required_data_size) {
				CommandError("Insufficient data space for tileset");
			}
			DefineTileset(cmd->tileset_index, num_entries, cmd->xdim, cmd->ydim, cmd->asset_index, uploaded_data_region);
#ifdef DEBUG
			std::cout << "SDHR_CMD_DEFINE_TILESET: Success! " << (uint32_t)cmd->tileset_index << ';'<< (uint32_t)num_entries << std::endl;
#endif
		} break;
		case SDHR_CMD_DEFINE_TILESET_IMMEDIATE: {
			if (!CheckCommandLength(p, end, sizeof(DefineTilesetImmediateCmd))) return false;
			DefineTilesetImmediateCmd* cmd = (DefineTilesetImmediateCmd*)p;
			uint16_t num_entries = cmd->num_entries;
			if (num_entries == 0) {
				num_entries = 256;
			}
			uint64_t load_data_size;
			load_data_size = (uint64_t)num_entries * 4;
			if (message_length != sizeof(DefineTilesetImmediateCmd) + load_data_size) {
				CommandError("DefineTilesetImmediate data size mismatch");
				return false;
			}
			DefineTileset(cmd->tileset_index, num_entries, cmd->xdim, cmd->ydim, cmd->asset_index, cmd->data);
#ifdef DEBUG
			std::cout << "SDHR_CMD_DEFINE_TILESET_IMMEDIATE: Success! " << (uint32_t)cmd->tileset_index << ';' << (uint32_t)num_entries << std::endl;
#endif
		} break;
		case SDHR_CMD_DEFINE_WINDOW: {
			if (!CheckCommandLength(p, end, sizeof(DefineWindowCmd))) return false;
			DefineWindowCmd* cmd = (DefineWindowCmd*)p;
			Window* r = windows + cmd->window_index;
			if (r->screen_xcount > screen_xcount) {
				CommandError("Window exceeds max x resolution");
				return false;
			}
			if (r->screen_ycount > screen_ycount) {
				CommandError("Window exceeds max y resolution");
				return false;
			}
			r->enabled = false;
			r->screen_xcount = cmd->screen_xcount;
			r->screen_ycount = cmd->screen_ycount;
			r->screen_xbegin = 0;
			r->screen_ybegin = 0;
			r->tile_xbegin = 0;
			r->tile_ybegin = 0;
			r->tile_xdim = cmd->tile_xdim;
			r->tile_ydim = cmd->tile_ydim;
			r->tile_xcount = cmd->tile_xcount;
			r->tile_ycount = cmd->tile_ycount;
			if (r->mesh) {
				delete r->mesh;
			}
			r->mesh = new MosaicMesh(r->tile_xcount, r->tile_ycount, r->tile_xdim, r->tile_ydim, cmd->window_index);
			r->mesh->shaderProgram = &defaultWindowShaderProgram;
			// Calculate the position of the mesh with respect to the screen top-left 0,0
			r->mesh->SetWorldCoordinates(r->screen_xbegin - r->tile_xbegin, r->screen_ybegin - r->tile_ybegin);

#ifdef DEBUG
			std::cout << "SDHR_CMD_DEFINE_WINDOW: Success! " 
				<< cmd->window_index << ';' << (uint32_t)r->tile_xcount << ';' << (uint32_t)r->tile_ycount << std::endl;
#endif
		} break;
		case SDHR_CMD_UPDATE_WINDOW_SET_IMMEDIATE: {
			size_t cmd_sz = sizeof(UpdateWindowSetImmediateCmd);
			if (!CheckCommandLength(p, end, cmd_sz)) return false;
			UpdateWindowSetImmediateCmd* cmd = (UpdateWindowSetImmediateCmd*)p;
			Window* r = windows + cmd->window_index;

			// full tile specification: tileset and index
			uint64_t required_data_size = (uint64_t)r->tile_xcount * r->tile_ycount * 2;
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
			for (uint64_t i = 0; i < cmd->data_length / 2; ++i) {
				uint8_t tileset_index = sp[i * 2];
				uint8_t tile_index = sp[i * 2 + 1];
				const TilesetRecord tr = tileset_records[tileset_index];
				if (tr.xdim != r->tile_xdim ||
					tr.ydim != r->tile_ydim ||
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
			Window* r = windows + cmd->window_index;
			// full tile specification: tileset and index
			uint64_t data_size = (uint64_t)cmd->block_count * 512;
			std::stringstream ss;
			upload_inflate((const char*)uploaded_data_region, data_size, ss);
			std::string s = ss.str();
			if (s.length() != r->tile_xcount * r->tile_ycount * 2) {
				CommandError("UploadWindowSetUpload data insufficient to define window tiles");
			}
			//  Allocate to each vertex:
			//  u, v coordinates of the texture (based on the tileset's tile index)
			//  textureId of the image asset used in the tileset
			//  NOTE: U/V has its 0,0 origin at the top left. OpenGL is bottom left
			uint8_t* sp = (uint8_t*)s.c_str();
			auto mesh = r->mesh;
			for (uint64_t tile_y = 0; tile_y < r->tile_ycount; ++tile_y) {
				// uint64_t line_offset = (uint64_t)tile_y * r->tile_xcount;
				for (uint64_t tile_x = 0; tile_x < r->tile_xcount; ++tile_x) {
					uint8_t tileset_index = *sp++;
					uint8_t tile_index = *sp++;
					const TilesetRecord tr = tileset_records[tileset_index];
					if (tr.xdim != r->tile_xdim ||
						tr.ydim != r->tile_ydim ||
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
			Window* r = windows + cmd->window_index;
			if ((uint64_t)cmd->tile_xbegin + cmd->tile_xcount > r->tile_xcount ||
				(uint64_t)cmd->tile_ybegin + cmd->tile_ycount > r->tile_ycount) {
				CommandError("tile update region exceeds tile dimensions");
				return false;
			}
			// partial tile specification: index and palette, single tileset
			uint64_t data_size = (uint64_t)cmd->tile_xcount * cmd->tile_ycount;
			if (data_size + sizeof(UpdateWindowSingleTilesetCmd) != message_length) {
				CommandError("UpdateWindowSingleTileset data size mismatch");
				return false;
			}
			uint8_t* dp = cmd->data;
			for (uint64_t tile_y = 0; tile_y < cmd->tile_ycount; ++tile_y) {
				uint64_t line_offset = (cmd->tile_ybegin + tile_y) * r->tile_xcount + cmd->tile_xbegin;
				for (uint64_t tile_x = 0; tile_x < cmd->tile_xcount; ++tile_x) {
					uint8_t tile_index = *dp++;
					if (tileset_records[cmd->tileset_index].xdim != r->tile_xdim ||
						tileset_records[cmd->tileset_index].ydim != r->tile_ydim ||
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
			Window* r = windows + cmd->window_index;
			if (cmd->x_dir < -1 || cmd->x_dir > 1 || cmd->y_dir < -1 || cmd->y_dir > 1) {
				CommandError("invalid tile shift");
				return false;
			}
			if (r->tile_xcount == 0 || r->tile_ycount == 0) {
				CommandError("invalid window for tile shift");
				return false;
			}

			r->tile_xbegin += cmd->x_dir;
			r->tile_ybegin += cmd->y_dir;

			r->tile_xbegin %= r->tile_xcount;
			r->tile_ybegin %= r->tile_ycount;
#if 0
			if (cmd->x_dir == -1) {
				for (uint64_t y_index = 0; y_index < r->tile_ycount; ++y_index) {
					uint64_t line_offset = y_index * r->tile_xcount;
					for (uint64_t x_index = 1; x_index < r->tile_xcount; ++x_index) {
						r->tilesets[line_offset + x_index - 1] = r->tilesets[line_offset + x_index];
						r->tile_indexes[line_offset + x_index - 1] = r->tile_indexes[line_offset + x_index];
					}
				}
			}
			else if (cmd->x_dir == 1) {
				for (uint64_t y_index = 0; y_index < r->tile_ycount; ++y_index) {
					uint64_t line_offset = y_index * r->tile_xcount;
					for (uint64_t x_index = r->tile_xcount - 1; x_index > 0; --x_index) {
						r->tilesets[line_offset + x_index] = r->tilesets[line_offset + x_index - 1];
						r->tile_indexes[line_offset + x_index] = r->tile_indexes[line_offset + x_index - 1];
					}
				}
			}
			if (cmd->y_dir == -1) {
				for (uint64_t y_index = 1; y_index < r->tile_ycount; ++y_index) {
					uint64_t line_offset = y_index * r->tile_xcount;
					uint64_t prev_line_offset = line_offset - r->tile_xcount;
					for (uint64_t x_index = 0; x_index < r->tile_xcount; ++x_index) {
						r->tilesets[prev_line_offset + x_index] = r->tilesets[line_offset + x_index];
						r->tile_indexes[prev_line_offset + x_index] = r->tile_indexes[line_offset + x_index];
					}
				}
			}
			else if (cmd->y_dir == 1) {
				for (uint64_t y_index = r->tile_ycount - 1; y_index > 0; --y_index) {
					uint64_t line_offset = y_index * r->tile_xcount;
					uint64_t prev_line_offset = line_offset - r->tile_xcount;
					for (uint64_t x_index = 0; x_index < r->tile_xcount; ++x_index) {
						r->tilesets[line_offset + x_index] = r->tilesets[prev_line_offset + x_index];
						r->tile_indexes[line_offset + x_index] = r->tile_indexes[prev_line_offset + x_index];
					}
				}
			}
#endif
			// Calculate the position of the mesh with respect to the screen top-left 0,0
			r->mesh->SetWorldCoordinates(r->screen_xbegin - r->tile_xbegin, r->screen_ybegin - r->tile_ybegin);
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_SHIFT_TILES: Success! " 
				<< (uint32_t)cmd->window_index << ';' << (uint32_t)cmd->x_dir << ';' << (uint32_t)cmd->y_dir << std::endl;
#endif
		} break;
		case SDHR_CMD_UPDATE_WINDOW_SET_WINDOW_POSITION: {
			if (!CheckCommandLength(p, end, sizeof(UpdateWindowSetWindowPositionCmd))) return false;
			UpdateWindowSetWindowPositionCmd* cmd = (UpdateWindowSetWindowPositionCmd*)p;
			Window* r = windows + cmd->window_index;
			r->screen_xbegin = cmd->screen_xbegin;
			r->screen_ybegin = cmd->screen_ybegin;
			// Here we don't change the mesh world_x/y because it's only the window that moves around the screen
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_SET_WINDOW_POSITION: Success! "
				<< (uint32_t)cmd->window_index << ';' << (uint32_t)cmd->screen_xbegin << ';' << (uint32_t)cmd->screen_ybegin << std::endl;
#endif
		} break;
		case SDHR_CMD_UPDATE_WINDOW_ADJUST_WINDOW_VIEW: {
			if (!CheckCommandLength(p, end, sizeof(UpdateWindowAdjustWindowViewCommand))) return false;
			UpdateWindowAdjustWindowViewCommand* cmd = (UpdateWindowAdjustWindowViewCommand*)p;
			Window* r = windows + cmd->window_index;
			r->tile_xbegin = cmd->tile_xbegin;
			r->tile_ybegin = cmd->tile_ybegin;
			// Calculate the position of the mesh with respect to the screen top-left 0,0
			r->mesh->SetWorldCoordinates(r->screen_xbegin - r->tile_xbegin, r->screen_ybegin - r->tile_ybegin);
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_ADJUST_WINDOW_VIEW: Success! "
				<< (uint32_t)cmd->window_index << ';' << (uint32_t)cmd->tile_xbegin << ';' << (uint32_t)cmd->tile_ybegin << std::endl;
#endif
		} break;
		case SDHR_CMD_UPDATE_WINDOW_ENABLE: {
			if (!CheckCommandLength(p, end, sizeof(UpdateWindowEnableCmd))) return false;
			UpdateWindowEnableCmd* cmd = (UpdateWindowEnableCmd*)p;
			Window* r = windows + cmd->window_index;
			if (!r->tile_xcount || !r->tile_ycount) {
				CommandError("cannote enable empty window");
				return false;
			}
			r->enabled = cmd->enabled;
#ifdef DEBUG
			std::cout << "SDHR_CMD_UPDATE_WINDOW_ENABLE: Success! "
				<< (uint32_t)cmd->window_index << std::endl;
#endif
		} break;
		default:
			CommandError("unrecognized command");
			return false;
		}
		p += message_length - 3;
	}
	return true;
}
