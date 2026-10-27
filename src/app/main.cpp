// NifMerge entry point. No arguments: GUI. With arguments: command line (used by the tests).
#include "Gui.hpp"
#include "core/Merge.hpp"
#include "core/NifIO.hpp"
#include "core/Pipeline.hpp"
#include "core/TextUtil.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

const char* kUsage =
	"Usage:\n"
	"  NifMerge                                   open the GUI\n"
	"  NifMerge merge --skeleton <skeleton.nif> --part <part.nif> [--part ...] [--wear <item.nif> ...]\n"
	"                 [--weight <0..100>] [--tri <file.tri | folder> ...] --out <model.fbx> [--obj <debug.obj>]\n"
	"      _0/_1 weight files are paired automatically; listing either one is enough.\n"
	"      --wear adds an outfit item: it replaces the character's parts in the body slots it takes.\n"
	"      --tri adds facial morphs (blend shapes): each .tri is matched to a head part by its geometry.\n"
	"  NifMerge inspect <file.nif> [--skeleton <skeleton.nif>]\n";

void ConsoleLog(nm::LogLevel level, const std::string& msg) {
	const char* tag = level == nm::LogLevel::Error ? "ERROR: " : level == nm::LogLevel::Warning ? "WARNING: " : "";
	std::cout << tag << msg << "\n";
}

int RunCli(const std::vector<std::string>& args) {
	const std::string cmd = args[0];
	auto value = [&](size_t& i) -> std::string {
		if (i + 1 >= args.size()) throw nm::Error("Missing value after " + args[i]);
		return args[++i];
	};

	if (cmd == "merge") {
		nm::RunOptions opt;
		std::vector<std::string> parts, outfit;
		for (size_t i = 1; i < args.size(); i++) {
			const std::string& a = args[i];
			if (a == "--skeleton") opt.request.skeletonPath = value(i);
			else if (a == "--part") parts.push_back(value(i));
			else if (a == "--wear") outfit.push_back(value(i));
			else if (a == "--weight") opt.request.weight = std::stod(value(i));
			else if (a == "--out") opt.fbxPath = value(i);
			else if (a == "--obj") opt.objPath = value(i);
			else if (a == "--tri") opt.request.morphPaths.push_back(value(i));
			else throw nm::Error("Unknown option: " + a);
		}
		if (opt.request.weight < 0 || opt.request.weight > 100) throw nm::Error("--weight must be between 0 and 100");
		opt.request.parts = nm::PairPartFiles(parts);
		opt.request.outfit = nm::PairPartFiles(outfit);
		nm::RunPipeline(opt, ConsoleLog);
		return 0;
	}
	if (cmd == "inspect") {
		std::string file, skelPath;
		for (size_t i = 1; i < args.size(); i++) {
			if (args[i] == "--skeleton") skelPath = value(i);
			else if (file.empty()) file = args[i];
			else throw nm::Error("Unexpected argument: " + args[i]);
		}
		if (file.empty()) throw nm::Error("inspect needs a nif file");
		nm::Skeleton skel;
		if (!skelPath.empty()) skel = nm::LoadSkeleton(skelPath);
		std::cout << nm::InspectNif(file, skelPath.empty() ? nullptr : &skel);
		return 0;
	}
	std::cout << kUsage;
	return cmd == "--help" || cmd == "-h" ? 0 : 2;
}

// Started from Explorer (double-click or files dropped on the exe): drop the console window that
// the console subsystem created. Started from a terminal: keep it.
void DetachOwnConsole() {
	DWORD procs[2];
	if (GetConsoleProcessList(procs, 2) <= 1) FreeConsole();
}

bool AllNifFiles(const std::vector<std::string>& args) {
	for (auto& a : args) {
		std::filesystem::path p = std::filesystem::u8path(a);
		std::string ext = p.extension().u8string();
		for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		if (ext != ".nif" || !std::filesystem::is_regular_file(p)) return false;
	}
	return true;
}

GuiInit ParseGuiArgs(const std::vector<std::string>& args) {
	GuiInit init;
	for (size_t i = 1; i < args.size(); i++) {
		const std::string& a = args[i];
		auto value = [&]() -> std::string {
			if (i + 1 >= args.size()) throw nm::Error("Missing value after " + a);
			return args[++i];
		};
		if (a == "--skeleton") init.skeleton = value();
		else if (a == "--part") init.files.push_back(value());
		else if (a == "--wear") init.outfit.push_back(value());
		else if (a == "--tri") init.morphPaths.push_back(value());
		else if (a == "--out") init.output = value();
		else if (a == "--weight") init.weight = std::stof(value());
		else if (a == "--go") init.go = true;
		else if (a == "--exit-when-done") init.exitWhenDone = true;
		else throw nm::Error("Unknown option: " + a);
	}
	return init;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) args.push_back(nm::ToUtf8(argv[i]));
	if (args.empty() || AllNifFiles(args)) {
		DetachOwnConsole();
		GuiInit init;
		init.files = args;
		return RunGui(init);
	}
	SetConsoleOutputCP(CP_UTF8);
	try {
		if (args[0] == "gui") return RunGui(ParseGuiArgs(args));
		return RunCli(args);
	}
	catch (const std::exception& e) {
		ConsoleLog(nm::LogLevel::Error, e.what());
		return 1;
	}
}
