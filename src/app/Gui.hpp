#pragma once
#include <string>
#include <vector>

struct GuiInit {
	std::vector<std::string> files; // .nif files: names containing "skeleton" fill the skeleton field, the rest are parts
	std::string skeleton;           // explicit skeleton, whatever its name
	std::vector<std::string> morphPaths; // .tri files or folders of them
	std::string output;
	float weight = 50.0f;
	bool go = false;           // press Go! on the first frame
	bool exitWhenDone = false; // close the window when that run finishes; exit code 1 if it failed
};

// Opens the Dear ImGui window and runs until it is closed. Returns the process exit code.
int RunGui(const GuiInit& init = {});
