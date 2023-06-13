#pragma once
#ifndef OPENGLHELPER_H
#define OPENGLHELPER_H
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include "common.h"
#include "shader.h"

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

	void load_texture(unsigned char* data, int width, int height, int nrComponents, GLuint textureID);
	size_t get_output_texture_id() { return output_texture_id; };	// output texture id
	size_t get_texture_id_at_slot(uint8_t slot);	// returns the opengl-generated texture id for this tex slot

	// TODO: Testing, remove
	// void create_vertices();
	void create_framebuffer(uint32_t width, uint32_t height);	// also binds it
	void bind_framebuffer();
	void unbind_framebuffer();
	void rescale_framebuffer(uint32_t width, uint32_t height);
	void get_framebuffer_size(uint32_t* width, uint32_t* height);
	void setup_render();
	void cleanup_render();

	// TODO: Testing, remove
	// void render();

	// The created texture ids (max is _SDHR_MAX_TEXTURES)
	std::vector<GLuint>v_texture_ids;
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

	GLuint output_texture_id;
//	GLuint VAO;	// for testing
//	GLuint VBO;	// for testing
	GLuint FBO = UINT_MAX;

	uint32_t fb_width = 0;
	uint32_t fb_height = 0;

};
#endif // OPENGLHELPER_H