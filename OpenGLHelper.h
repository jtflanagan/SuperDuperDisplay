#pragma once
#ifndef OPENGLHELPER_H
#define OPENGLHELPER_H
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include "common.h"
#include "shader.h"
#include "camera.h"

class OpenGLHelper
{
public:
	// public singleton code
	static OpenGLHelper* GetInstance()
	{
		if (NULL == s_instance)
			s_instance = new OpenGLHelper();
		return s_instance;
	}
	~OpenGLHelper();

	// METHODS THAT CAN ONLY BE CALLED FROM THE MAIN THREAD
	void load_texture(unsigned char* data, int width, int height, int nrComponents, GLuint textureID);
	size_t get_output_texture_id() { return output_texture_id; };	// output texture id
	size_t get_texture_id_at_slot(uint8_t slot);	// returns the opengl-generated texture id for this tex slot
	void create_framebuffer(uint32_t width, uint32_t height);	// also binds it
	void bind_framebuffer();
	void unbind_framebuffer();
	void rescale_framebuffer(uint32_t width, uint32_t height);
	void setup_render();
	void cleanup_render();

	// METHODS THAT CAN BE CALLED FROM ANY THREAD
	bool request_framebuffer_resize(uint32_t width, uint32_t height);
	void get_framebuffer_size(uint32_t* width, uint32_t* height);
	void set_callback_changed_resolution(void(*func)(int w, int h)) { callbackResolutionChange = func; };
	uint32_t get_frame_ticks();	// get the global tick value for the current frame

	// The created texture ids (max is _SDHR_MAX_TEXTURES)
	std::vector<GLuint>v_texture_ids;

	// Camera for World -> View matrix transform
	Camera camera = Camera(
		_SCREEN_DEFAULT_WIDTH / 2.f, _SCREEN_DEFAULT_HEIGHT / 2.f,			// x,y
		UINT8_MAX,									// z
		0.f, 1.f, 0.f,								// upVector xyz
		-90.f,										// yaw
		0.f											// pitch
	);
	// Projection matrix (left, right, bottom, top, near, far)
	glm::mat4 mat_proj = glm::ortho<float>(
		-(float)_SCREEN_DEFAULT_WIDTH / 2, (float)_SCREEN_DEFAULT_WIDTH / 2,
		-(float)_SCREEN_DEFAULT_HEIGHT / 2, (float)_SCREEN_DEFAULT_HEIGHT / 2,
		0, 256);

	// Debugging attributes
	bool bDebugNoTextures = false;
	bool bUsePerspective = false;		// see bIsUsingPerspective

private:
//////////////////////////////////////////////////////////////////////////
// Singleton pattern
//////////////////////////////////////////////////////////////////////////
	void Initialize();

	static OpenGLHelper* s_instance;
	OpenGLHelper()
	{
		Initialize();
	}

	void (*callbackResolutionChange)(int w, int h);

	GLuint output_texture_id;
//	GLuint VAO;	// for testing
//	GLuint VBO;	// for testing
	GLuint FBO = UINT_MAX;

	uint32_t fb_width = _SCREEN_DEFAULT_WIDTH;
	uint32_t fb_height = _SCREEN_DEFAULT_HEIGHT;

	uint32_t fb_width_requested = UINT32_MAX;
	uint32_t fb_height_requested = UINT32_MAX;

	uint32_t frame_ticks;

	//////////////////////////////////////////////////////////////////////////
	// Internal attributes
	//////////////////////////////////////////////////////////////////////////
	bool bIsUsingPerspective = false;	// is it currently using perspective?
	bool bDidChangeResolution = false;	// did the resolution change?
};
#endif // OPENGLHELPER_H
