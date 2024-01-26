#pragma once
#ifndef POSTPROCESSOR_H
#define POSTPROCESSOR_H

/*
	Singleton postprocessing class whose job is to:
		-remember if there needs to be postprocessing
		- decide which shaders need to be used for postprocessing and in which order
		- provide the necessary uniforms(parameters) to the shaders
		- provide an ImGui interface to:
			- enable/disable postprocessing
			- choose up to 5 shaders from the filesystem

*/

#include "common.h"
#include "shader.h"
#include <array>

class PostProcessor
{
public:
	// public singleton code
	static PostProcessor* GetInstance()
	{
		if (NULL == s_instance)
			s_instance = new PostProcessor();
		return s_instance;
	}
	~PostProcessor();

	void Render();
	void DisplayImGuiPPWindow(bool* p_open);

	// public properties
	bool enabled = false;
	std::array<Shader, 5>v_ppshaders;
private:
	//////////////////////////////////////////////////////////////////////////
	// Singleton pattern
	//////////////////////////////////////////////////////////////////////////
	void Initialize();

	static PostProcessor* s_instance;
	PostProcessor()
	{
		Initialize();
	}
};

#endif	// POSTPROCESSOR_H