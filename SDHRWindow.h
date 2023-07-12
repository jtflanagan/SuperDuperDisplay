#pragma once
#ifndef SDHRWINDOW_H
#define SDHRWINDOW_H
#include "MosaicMesh.h"
#include "shader.h"

class SDHRWindow
{
public:
	bool enabled;
	bool black_or_wrap;      // false: viewport is black outside of tile range, true: viewport wraps
	// TODO: Enable black_or_wrap functionality. Currently only wrap.
	int anim_ms_frame = 0;	// number of ms to animate each frame

	SDHRWindow()
		: enabled(false), black_or_wrap(true)
		, index(UINT8_MAX)
		, screen_count(uXY({ 0,0 }))
		, screen_begin(iXY({ 0,0 }))
		, tile_begin(iXY({ 0,0 }))
		, tile_dim(uXY({ 0,0 }))
		, tile_count(uXY({ 0,0 }))
	{};

	~SDHRWindow()
	{
		if (mesh) {
			delete mesh;
			mesh = nullptr;
		}
	};

	MosaicMesh* mesh = nullptr;

	void Define(uXY _screen_count, uXY _tile_dim, uXY _tile_count, Shader* _shaderProgram);
	void ShiftTiles(iXY _direction);
	void SetPosition(iXY _screen_pos);	// Sets the window position on screen
	void AdjustView(iXY _mesh_pos);		// Adjusts the mesh position relative to the window
	void SetSize(uXY _size);			// Sets the window size

	void Update();
	void Render(const glm::mat4& mat_camera, const glm::mat4& mat_proj);
	void Reset();

	bool IsEmpty() { return (tile_count.x == 0 || tile_count.y == 0); };

	uint8_t Get_index() const { return index; }
	void Set_index(uint8_t val) { index = val; }
	uXY Get_screen_count() const { return screen_count; }
	iXY Get_screen_begin() const { return screen_begin; }
	iXY Get_tile_begin() const { return tile_begin; }
	uXY Get_tile_dim() const { return tile_dim; }
	uXY Get_tile_count() const { return tile_count; }

private:
	uint8_t index;		// index of window (is also the z-value: higher is closer to camera)
	uXY screen_count;	// width in pixels of visible screen area of window
	iXY screen_begin;	// pixel xy coordinate where window begins
	iXY tile_begin;		// pixel xy coordinate on backing tile array where aperture begins
	uXY tile_dim;		// xy dimension, in pixels, of tiles in the window.
	uXY tile_count;		// xy dimension, in tiles, of the tile array

};

#endif // SDHRWINDOW_H