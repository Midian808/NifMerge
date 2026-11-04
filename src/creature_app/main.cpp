// SkyCreature entry point. No arguments, or only existing files and folders (dropped on the exe): GUI.
// Otherwise a command line (used by the tests).
#include "CreatureGui.hpp"
#include "core/TextUtil.hpp"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const char* kUsage =
	"Usage:\n"
	"  SkyCreature                                 open the GUI\n"
	"  SkyCreature export [--skeleton <skeleton.nif>] [--hkx <skeleton.hkx>] [--part <model.nif>]...\n"
	"                     [--rig auto|humanoid|generic] [--name <name>] <file|folder>... --out <folder>\n"
	"      Writes <out>\\<name>.fbx + .materials.json (the creature's model parts merged on its skeleton) and\n"
	"      <out>\\anims\\ with one .fbx + .anim.json per clip. Inputs are sorted by content: a .nif file without\n"
	"      shapes is the skeleton, one with shapes a model part; folders are searched for .hkx (skeleton, clips).\n"
	"      --rig auto (default): Humanoid when the skeleton has every Humanoid bone, else Generic.\n"
	"  SkyCreature gui [the export options] [--go] [--exit-when-done] <file|folder>...\n";

void ConsoleLog(nm::LogLevel level, const std::string& msg) {
	const char* tag = level == nm::LogLevel::Error ? "ERROR: " : level == nm::LogLevel::Warning ? "WARNING: " : "";
	std::cout << tag << msg << "\n";
}

nm::CreatureRig ParseRig(const std::string& v) {
	if (v == "auto") return nm::CreatureRig::Auto;
	if (v == "humanoid") return nm::CreatureRig::Humanoid;
	if (v == "generic") return nm::CreatureRig::Generic;
	throw nm::Error("--rig must be auto, humanoid or generic, not '" + v + "'");
}

// The export options, shared by `export` and `gui`. Returns false for an argument it does not know.
bool ParseOption(const std::vector<std::string>& args, size_t& i, nm::CreatureRequest& req) {
	const std::string& a = args[i];
	auto value = [&]() -> std::string {
		if (i + 1 >= args.size()) throw nm::Error("Missing value after " + a);
		return args[++i];
	};
	if (a == "--skeleton") req.skeletonNif = value();
	else if (a == "--hkx") req.skeletonHkx = value();
	else if (a == "--part") req.parts.push_back(value());
	else if (a == "--rig") req.rig = ParseRig(value());
	else if (a == "--name") req.name = value();
	else if (a == "--out") req.outDir = value();
	else if (a.rfind("--", 0) == 0) return false;
	else req.inputs.push_back(a);
	return true;
}

int RunCli(const std::vector<std::string>& args) {
	if (args[0] == "export") {
		nm::CreatureRequest req;
		for (size_t i = 1; i < args.size(); i++)
			if (!ParseOption(args, i, req)) throw nm::Error("Unknown option: " + args[i]);
		nm::CreatureResult r = nm::RunCreature(req, ConsoleLog);
		return r.failed() ? 1 : 0;
	}
	std::cout << kUsage;
	return args[0] == "--help" || args[0] == "-h" ? 0 : 2;
}

CreatureGuiInit ParseGuiArgs(const std::vector<std::string>& args) {
	nm::CreatureRequest req;
	CreatureGuiInit init;
	for (size_t i = 1; i < args.size(); i++) {
		const std::string& a = args[i];
		if (a == "--go") init.go = true;
		else if (a == "--exit-when-done") init.exitWhenDone = true;
		else if (a == "--write-layout") {
			if (i + 1 >= args.size()) throw nm::Error("Missing value after " + a);
			init.layoutFile = args[++i];
		} else if (!ParseOption(args, i, req)) throw nm::Error("Unknown option: " + a);
	}
	init.inputs = req.inputs;
	init.parts = req.parts;
	init.skeletonNif = req.skeletonNif;
	init.skeletonHkx = req.skeletonHkx;
	init.rig = req.rig;
	init.name = req.name;
	init.outDir = req.outDir;
	return init;
}

// Started from Explorer (double-click or files dropped on the exe): drop the console window that
// the console subsystem created. Started from a terminal: keep it.
void DetachOwnConsole() {
	DWORD procs[2];
	if (GetConsoleProcessList(procs, 2) <= 1) FreeConsole();
}

bool AllExistingPaths(const std::vector<std::string>& args) {
	for (auto& a : args) {
		std::error_code ec;
		if (!fs::exists(fs::u8path(a), ec)) return false;
	}
	return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) args.push_back(nm::ToUtf8(argv[i]));
	if (args.empty() || AllExistingPaths(args)) {
		DetachOwnConsole();
		CreatureGuiInit init;
		init.inputs = args;
		return RunCreatureGui(init);
	}
	SetConsoleOutputCP(CP_UTF8);
	try {
		if (args[0] == "gui") return RunCreatureGui(ParseGuiArgs(args));
		return RunCli(args);
	} catch (const std::exception& e) {
		ConsoleLog(nm::LogLevel::Error, e.what());
		return 1;
	}
}
