// SkyAnim entry point. No arguments, or only existing files and folders (dropped on the exe): GUI.
// Otherwise a command line (used by the tests).
#include "AnimGui.hpp"
#include "anim/Batch.hpp"
#include "anim/HkxAnimation.hpp"
#include "core/TextUtil.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const char* kUsage =
	"Usage:\n"
	"  SkyAnim                                     open the GUI\n"
	"  SkyAnim export [--skeleton <skeleton.nif>] [--hkx <skeleton.hkx>] <file|folder>... --out <folder>\n"
	"      Exports every animation clip among the inputs (folders are searched recursively) as FBX + .anim.json.\n"
	"      The skeleton .nif and .hkx are recognised among the inputs by content; the options override that.\n"
	"  SkyAnim inspect <file.hkx>                  what a .hkx holds: skeletons, or a clip's tracks, encodings, events\n"
	"  SkyAnim scan <folder> [--list <encoding>]   encodings and classes used by every .hkx in a folder;\n"
	"                                              --list prints every file using one (e.g. \"float tracks\")\n"
	"  SkyAnim dump <clip.hkx> --out <file.tsv>    decoded transforms per frame and track (test oracle input)\n"
	"  SkyAnim gui [--skeleton <nif>] [--hkx <hkx>] [--out <folder>] [--go] [--exit-when-done] <file|folder>...\n";

void ConsoleLog(nm::LogLevel level, const std::string& msg) {
	const char* tag = level == nm::LogLevel::Error ? "ERROR: " : level == nm::LogLevel::Warning ? "WARNING: " : "";
	std::cout << tag << msg << "\n";
}

std::string Join(const std::set<std::string>& items) {
	std::string r;
	for (auto& s : items) r += (r.empty() ? "" : ", ") + s;
	return r.empty() ? "-" : r;
}

int Inspect(const std::string& path) {
	nm::HkxPackfile pf = nm::HkxPackfile::Load(path);
	std::cout << path << "\n  Havok " << pf.version() << ", " << (pf.pointerSize() == 8 ? "64-bit (Skyrim SE)" : "32-bit (Skyrim LE)")
			  << ", root " << pf.rootClass() << "\n";
	std::map<std::string, int> classes;
	for (auto& [off, cls] : pf.objects()) classes[cls]++;
	std::cout << "  objects:";
	for (auto& [cls, n] : classes) std::cout << " " << cls << (n > 1 ? " x" + std::to_string(n) : "");
	std::cout << "\n";
	switch (nm::Classify(pf)) {
		case nm::HkxKind::Skeleton:
			for (auto& s : nm::ReadSkeletons(pf)) std::cout << "  skeleton '" << s.name << "': " << s.bones.size() << " bones\n";
			break;
		case nm::HkxKind::Animation: {
			nm::HkxClip c = nm::ReadClip(pf);
			int named = 0;
			for (auto& n : c.trackNames) named += n && !n->empty();
			std::cout << "  " << c.animClass << ": " << c.numFrames << " frames, " << nm::JsonNum(c.duration) << " s, "
					  << nm::JsonNum(1.0 / c.frameDuration) << " fps\n";
			std::cout << "  tracks: " << c.numTracks << " transform (" << named << " named), " << c.numFloatTracks << " float\n";
			std::cout << "  binding: skeleton '" << c.skeletonName << "', "
					  << (c.trackToBone.empty() ? "no index map" : std::to_string(c.trackToBone.size()) + " indices")
					  << (c.blendHint ? ", additive" : "") << "\n";
			std::cout << "  root motion: " << (!c.hasReferenceFrame ? "none" : c.referenceFrameMoves ? "moves" : "present, all zero") << "\n";
			std::cout << "  encodings: " << Join(c.formats) << "\n  unverified: " << Join(c.unverifiedFormats) << "\n";
			for (auto& e : c.events) std::cout << "  event " << nm::JsonNum(e.time) << " s: " << nm::JsonQuote(e.text) << "\n";
			break;
		}
		default:
			break;
	}
	return 0;
}

int Scan(const std::string& folder, const std::string& list) {
	std::map<std::string, std::vector<std::string>> byFormat; // encoding -> files using it
	std::map<std::string, int> roots;
	int files = 0, failed = 0;
	std::error_code ec;
	for (auto& e : fs::recursive_directory_iterator(fs::u8path(folder), fs::directory_options::skip_permission_denied, ec)) {
		std::string ext = e.path().extension().u8string();
		for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (!e.is_regular_file(ec) || ext != ".hkx") continue;
		files++;
		const std::string path = e.path().u8string();
		try {
			nm::HkxPackfile pf = nm::HkxPackfile::Load(path);
			std::string platform = pf.pointerSize() == 8 ? "SE " : "LE ";
			if (nm::Classify(pf) != nm::HkxKind::Animation) {
				roots[platform + pf.rootClass()]++;
				continue;
			}
			roots[platform + "animation"]++;
			nm::HkxClip c = nm::ReadClip(pf);
			for (auto& f : c.formats) byFormat[platform + f].push_back(path);
		} catch (const std::exception& ex) {
			failed++;
			byFormat["unreadable: " + std::string(ex.what()).substr(0, 120)].push_back(path);
		}
	}
	if (!list.empty()) {
		for (auto& [f, paths] : byFormat)
			if (f == list || f.substr(3) == list)
				for (auto& p : paths) std::cout << p << "\n";
		return 0;
	}
	std::cout << files << " .hkx files, " << failed << " unreadable\n";
	for (auto& [r, n] : roots) std::cout << "  " << n << "  " << r << "\n";
	std::cout << "Encodings (count, first file):\n";
	for (auto& [f, list] : byFormat) std::cout << "  " << list.size() << "  " << f << "   e.g. " << list.front() << "\n";
	return 0;
}

int Dump(const std::string& path, const std::string& out) {
	nm::HkxClip c = nm::ReadClip(nm::HkxPackfile::Load(path));
	std::ofstream f(fs::u8path(out), std::ios::binary);
	if (!f) throw nm::Error("Cannot write " + out);
	char buf[512];
	f << "frame\ttrack\ttx\tty\ttz\tqx\tqy\tqz\tqw\tsx\tsy\tsz\n";
	for (int fr = 0; fr < c.numFrames; fr++)
		for (int t = 0; t < c.numTracks; t++) {
			const auto& x = c.frames[fr][t];
			snprintf(buf, sizeof buf, "%d\t%d\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\t%.9g\n", fr, t, x.t.x, x.t.y, x.t.z,
					 x.r.x, x.r.y, x.r.z, x.r.w, x.s.x, x.s.y, x.s.z);
			f << buf;
		}
	return 0;
}

int RunCli(const std::vector<std::string>& args) {
	const std::string cmd = args[0];
	auto value = [&](size_t& i) -> std::string {
		if (i + 1 >= args.size()) throw nm::Error("Missing value after " + args[i]);
		return args[++i];
	};
	if (cmd == "export") {
		nm::BatchRequest req;
		for (size_t i = 1; i < args.size(); i++) {
			const std::string& a = args[i];
			if (a == "--skeleton") req.skeletonNif = value(i);
			else if (a == "--hkx") req.skeletonHkx = value(i);
			else if (a == "--out") req.outDir = value(i);
			else if (a.rfind("--", 0) == 0) throw nm::Error("Unknown option: " + a);
			else req.inputs.push_back(a);
		}
		nm::BatchResult r = nm::RunBatch(req, ConsoleLog);
		return r.failed() ? 1 : 0;
	}
	if (cmd == "inspect" && args.size() == 2) return Inspect(args[1]);
	if (cmd == "scan" && args.size() == 2) return Scan(args[1], "");
	if (cmd == "scan" && args.size() == 4 && args[2] == "--list") return Scan(args[1], args[3]);
	if (cmd == "dump" && args.size() == 4 && args[2] == "--out") return Dump(args[1], args[3]);
	std::cout << kUsage;
	return cmd == "--help" || cmd == "-h" ? 0 : 2;
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

AnimGuiInit ParseGuiArgs(const std::vector<std::string>& args) {
	AnimGuiInit init;
	for (size_t i = 1; i < args.size(); i++) {
		const std::string& a = args[i];
		auto value = [&]() -> std::string {
			if (i + 1 >= args.size()) throw nm::Error("Missing value after " + a);
			return args[++i];
		};
		if (a == "--skeleton") init.skeletonNif = value();
		else if (a == "--hkx") init.skeletonHkx = value();
		else if (a == "--out") init.outDir = value();
		else if (a == "--go") init.go = true;
		else if (a == "--exit-when-done") init.exitWhenDone = true;
		else if (a == "--write-layout") init.layoutFile = value();
		else if (a.rfind("--", 0) == 0) throw nm::Error("Unknown option: " + a);
		else init.inputs.push_back(a);
	}
	return init;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) args.push_back(nm::ToUtf8(argv[i]));
	if (args.empty() || AllExistingPaths(args)) {
		DetachOwnConsole();
		AnimGuiInit init;
		init.inputs = args;
		return RunAnimGui(init);
	}
	SetConsoleOutputCP(CP_UTF8);
	try {
		if (args[0] == "gui") return RunAnimGui(ParseGuiArgs(args));
		return RunCli(args);
	} catch (const std::exception& e) {
		ConsoleLog(nm::LogLevel::Error, e.what());
		return 1;
	}
}
