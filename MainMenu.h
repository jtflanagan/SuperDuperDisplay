#ifndef MAINMENU_H
#define MAINMENU_H

#include <SDL.h>

class MainMenu {
public:
	MainMenu(SDL_GLContext gl_context, SDL_Window* window);
	~MainMenu();
	void Render();
	bool HandleEvent(SDL_Event& event);
	
private:
	void ShowSDDMenu();
	void ShowMotherboardMenu();
	void ShowVideoMenu();
	void ShowSoundMenu();
	void ShowSamplesMenu();
	void ShowDeveloperMenu();
	
	void HandleQuit();
	
	SDL_GLContext gl_context_;
	SDL_Window* window_;
	
	class Gui;
	Gui* pGui;  // Hides ImGui details
};

#endif // MAINMENU_H
