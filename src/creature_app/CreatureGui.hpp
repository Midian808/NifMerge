#pragma once
#include "creature/Creature.hpp"

#include <string>
#include <vector>

struct CreatureGuiInit {
	std::vector<std::string> inputs; // files and folders, routed as if dropped on the window
	std::vector<std::string> parts;  // model parts given explicitly
	std::string skeletonNif, skeletonHkx, name, outDir;
	nm::CreatureRig rig = nm::CreatureRig::Auto;
	bool go = false;           // press Go! on the first frame
	bool exitWhenDone = false; // close when that run finishes; exit code 1 if it or any clip failed
	std::string layoutFile;    // for the GUI test: a drop point and Go! in client pixels
};

// Opens SkyCreature's window and runs until it is closed. Returns the process exit code.
int RunCreatureGui(const CreatureGuiInit& init = {});
