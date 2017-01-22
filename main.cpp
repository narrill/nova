#include <Windows.h>
#include <time.h>
#include <thread>
#include <iostream>
#include "Game.h"

DWORD WINAPI JobLoop(LPVOID lpParam) {
	while (true)
		printf("test");
}

// --------------------------------------------------------
// Entry point for a graphical (non-console) Windows application
// --------------------------------------------------------
int WINAPI WinMain(
	HINSTANCE hInstance,		// The handle to this app's instance
	HINSTANCE hPrevInstance,	// A handle to the previous instance of the app (always NULL)
	LPSTR lpCmdLine,			// Command line params
	int nCmdShow)				// How the window should be shown (we ignore this)
{
#if defined(DEBUG) | defined(_DEBUG)
	// Enable memory leak detection as a quick and dirty
	// way of determining if we forgot to clean something up
	//  - You may want to use something more advanced, like Visual Leak Detector
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
	srand(time(NULL));

	// Create the Game object using
	// the app handle we got from WinMain
	Game dxGame(hInstance);

	// Result variable for function calls below
	HRESULT hr = S_OK;

	// Attempt to create the window for our program, and
	// exit early if something failed
	hr = dxGame.InitWindow();
	if (FAILED(hr)) return hr;

	// Attempt to initialize DirectX, and exit
	// early if something failed
	hr = dxGame.InitDirectX();
	if (FAILED(hr)) return hr;

	//SetCapture(hInstance);

	// Begin the message and game loop, and then return
	// whatever we get back once the game loop is over
	//return dxGame.Run();

	for(unsigned int c = 0;c<std::thread::hardware_concurrency()-1;c++)
		CreateThread(
			NULL,                   // default security attributes
			0,                      // use default stack size  
			JobLoop,       // thread function name
			NULL,          // argument to thread function 
			0,                      // use default creation flags 
			NULL);   // returns the thread identifier 
	JobLoop(NULL);
}

/*static float fRand(float min, float max) {
float f = (float)rand() / RAND_MAX;
return min + f * (max - min);
}*/