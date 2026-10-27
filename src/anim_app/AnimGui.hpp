#pragma once
#include <string>
#include <vector>

struct AnimGuiInit {
	std::vector<std::string> inputs; // files and folders, routed as if dropped on the window
	std::string skeletonNif, skeletonHkx, outDir;
	bool go = false;           // press Go! on the first frame
	bool exitWhenDone = false; // close when that run finishes; exit code 1 if it or any clip failed
	std::string layoutFile;    // for the GUI test: the drop area and Go! in client pixels
};

// Opens SkyAnim's window and runs until it is closed. Returns the process exit code.
int RunAnimGui(const AnimGuiInit& init = {});
