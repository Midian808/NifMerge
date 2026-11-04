// SkyCreature's Dear ImGui front end: skeleton (.nif + .hkx), model parts, animations (files and
// folders), rig, name, output folder, Go!.
#include "CreatureGui.hpp"

#include "anim/HkxAnimation.hpp"
#include "app/GuiHost.hpp"
#include "core/NifIO.hpp"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace {

namespace fs = std::filesystem;

struct LogLine {
	nm::LogLevel level;
	std::string text;
};

struct AppState {
	std::string skeletonNif, skeletonHkx, name, outDir;
	std::vector<std::string> parts;  // model .nif files
	std::vector<std::string> inputs; // clips and folders
	int rig = 0;                     // 0 auto, 1 humanoid, 2 generic
	int selectedPart = -1, selectedInput = -1;

	std::mutex logMutex;
	std::vector<LogLine> log;
	bool scrollLog = false;
	std::atomic<bool> running{false};
	std::atomic<bool> lastRunFailed{false};
	std::atomic<int> runsFinished{0};
	std::thread worker;

	void Log(nm::LogLevel l, const std::string& s) {
		std::lock_guard<std::mutex> lock(logMutex);
		log.push_back({l, s});
		scrollLog = true;
	}
};

AppState g_app;
float g_partsBand[2] = {}; // model parts section: top, bottom (client pixels)
float g_goRect[4] = {};

const char* kRigNames[] = {"Auto (Humanoid when the skeleton has every Humanoid bone, else Generic)", "Humanoid", "Generic"};

nm::CreatureRig RigOf(int i) {
	return i == 1 ? nm::CreatureRig::Humanoid : i == 2 ? nm::CreatureRig::Generic : nm::CreatureRig::Auto;
}

std::string LowerExt(const std::string& path) {
	std::string e = fs::u8path(path).extension().u8string();
	for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return e;
}

void AddUnique(std::vector<std::string>& list, const std::string& p) {
	for (auto& existing : list)
		if (fs::u8path(existing).lexically_normal() == fs::u8path(p).lexically_normal()) return;
	list.push_back(p);
}

// Dropped or picked files and folders, routed by content: a .nif without shapes fills the skeleton, a
// .nif with shapes joins the model parts, a .hkx holding a skeleton fills the Havok skeleton, clips and
// folders join the animations. Two skeletons of one kind in one drop fill neither: the user picks one.
void AddPaths(const std::vector<std::string>& paths) {
	std::vector<std::string> nifs, hkxSkeletons;
	for (auto& p : paths) {
		std::error_code ec;
		if (fs::is_directory(fs::u8path(p), ec)) {
			AddUnique(g_app.inputs, p);
			continue;
		}
		const std::string ext = LowerExt(p);
		if (ext == ".nif") {
			try {
				if (nm::ReadShapeSlots(p).empty()) nifs.push_back(p);
				else AddUnique(g_app.parts, p);
			} catch (const std::exception& e) {
				g_app.Log(nm::LogLevel::Error, e.what());
			}
		} else if (ext == ".hkx") {
			nm::HkxKind kind = nm::HkxKind::Animation; // an unreadable file fails as a clip, with its reason
			try {
				kind = nm::Classify(nm::HkxPackfile::Load(p));
			} catch (const std::exception&) {
			}
			if (kind == nm::HkxKind::Skeleton) hkxSkeletons.push_back(p);
			else if (kind == nm::HkxKind::Animation) AddUnique(g_app.inputs, p);
			else g_app.Log(nm::LogLevel::Info, "Skipped (holds no animation or skeleton): " + p);
		} else {
			g_app.Log(nm::LogLevel::Warning, "Ignored (not a .nif, .hkx or folder): " + p);
		}
	}
	auto settle = [](const std::vector<std::string>& found, std::string& field, const char* what) {
		if (found.size() == 1) field = found[0];
		if (found.size() > 1) {
			std::string names;
			for (auto& f : found) names += "\n  " + f;
			g_app.Log(nm::LogLevel::Error, std::string("More than one ") + what + " dropped; choose one with Browse:" + names);
		}
	};
	settle(nifs, g_app.skeletonNif, "skeleton .nif");
	settle(hkxSkeletons, g_app.skeletonHkx, "skeleton .hkx");
	if (g_app.skeletonHkx.empty() && !g_app.skeletonNif.empty()) { // the .hkx usually sits beside the .nif
		fs::path nif = fs::u8path(g_app.skeletonNif);
		for (fs::path c : {nif.parent_path() / (nif.stem().u8string() + ".hkx"), nif.parent_path() / "skeleton.hkx"})
			if (fs::is_regular_file(c)) {
				g_app.skeletonHkx = c.u8string();
				break;
			}
	}
	if (g_app.outDir.empty() && !g_app.parts.empty())
		g_app.outDir = (fs::u8path(g_app.parts.front()).parent_path() / "fbx").u8string();
}

void WriteLayout(const std::string& path) {
	static std::string written;
	auto f = [](float v) { return std::to_string(static_cast<int>(v)); };
	std::string json = "{\"parts\": [" + f(g_partsBand[0]) + ", " + f(g_partsBand[1]) + "], \"go\": [" + f(g_goRect[0]) + ", " +
					   f(g_goRect[1]) + ", " + f(g_goRect[2]) + ", " + f(g_goRect[3]) + "]}\n";
	if (json == written) return;
	const fs::path target = fs::u8path(path), temp = fs::u8path(path + ".tmp");
	{
		std::ofstream out(temp, std::ios::binary);
		out << json;
		if (!out) return;
	}
	std::error_code ec;
	fs::rename(temp, target, ec);
	if (!ec) written = json;
}

void StartRun() {
	if (g_app.running) return;
	if (g_app.worker.joinable()) g_app.worker.join();
	nm::CreatureRequest req;
	req.inputs = g_app.inputs;
	req.parts = g_app.parts;
	req.skeletonNif = g_app.skeletonNif;
	req.skeletonHkx = g_app.skeletonHkx;
	req.rig = RigOf(g_app.rig);
	req.name = g_app.name;
	req.outDir = g_app.outDir;
	g_app.Log(nm::LogLevel::Info, "---- Go!");
	g_app.running = true;
	g_app.worker = std::thread([req] {
		bool failed = false;
		try {
			nm::CreatureResult r = nm::RunCreature(req, [](nm::LogLevel l, const std::string& s) { g_app.Log(l, s); });
			failed = r.failed();
		} catch (const std::exception& e) {
			g_app.Log(nm::LogLevel::Error, e.what());
			failed = true;
		}
		g_app.lastRunFailed = failed;
		g_app.runsFinished++;
		g_app.running = false;
	});
}

void PathField(const char* id, std::string& value, float browseW) {
	ImGui::SetNextItemWidth(-browseW - ImGui::GetStyle().ItemSpacing.x);
	ImGui::InputText(id, value.data(), value.size() + 1, ImGuiInputTextFlags_ReadOnly);
	ImGui::SameLine();
}

// A selectable list of paths; folders are marked.
void PathList(const char* id, std::vector<std::string>& list, int& selected, int rows) {
	if (ImGui::BeginListBox(id, ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * rows))) {
		for (int i = 0; i < static_cast<int>(list.size()); i++) {
			const std::string& p = list[i];
			std::error_code ec;
			std::string label = (fs::is_directory(fs::u8path(p), ec) ? "[folder] " : "") + p + "##" + id + std::to_string(i);
			if (ImGui::Selectable(label.c_str(), selected == i)) selected = i;
		}
		ImGui::EndListBox();
	}
}

void RemoveClear(const char* id, std::vector<std::string>& list, int& selected) {
	ImGui::SameLine();
	ImGui::BeginDisabled(selected < 0 || selected >= static_cast<int>(list.size()));
	if (ImGui::Button((std::string("Remove##") + id).c_str())) {
		list.erase(list.begin() + selected);
		selected = -1;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button((std::string("Clear##") + id).c_str())) {
		list.clear();
		selected = -1;
	}
}

void DrawUi(HWND hwnd) {
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::Begin("SkyCreature", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	const bool busy = g_app.running;
	const float browseW = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
	ImGui::BeginDisabled(busy);

	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("Drag files and folders onto this window; they are sorted by content.");
	ImGui::PopTextWrapPos();

	ImGui::SeparatorText("Skeleton");
	ImGui::TextDisabled("The creature's skeleton.nif and its Havok skeleton (.hkx, found beside the .nif when it is there).");
	PathField("##nif", g_app.skeletonNif, browseW);
	if (ImGui::Button("Browse...##nif")) {
		auto f = OpenFileDialog(hwnd, false, L"Choose the skeleton nif", {{L"NIF files", L"*.nif"}, {L"All files", L"*.*"}});
		if (!f.empty()) {
			g_app.skeletonNif = f[0];
			AddPaths({});
		}
	}
	PathField("##hkx", g_app.skeletonHkx, browseW);
	if (ImGui::Button("Browse...##hkx")) {
		auto f = OpenFileDialog(hwnd, false, L"Choose the skeleton hkx", {{L"Havok files", L"*.hkx"}, {L"All files", L"*.*"}});
		if (!f.empty()) g_app.skeletonHkx = f[0];
	}

	g_partsBand[0] = ImGui::GetCursorScreenPos().y;
	ImGui::SeparatorText("Model parts");
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("The creature's .nif files with shapes (body, mane, saddle ...), merged into one model with one material "
						"slot per shape.");
	ImGui::PopTextWrapPos();
	PathList("parts", g_app.parts, g_app.selectedPart, 5);
	if (ImGui::Button("Add files...##parts")) {
		auto f = OpenFileDialog(hwnd, true, L"Add model parts", {{L"NIF files", L"*.nif"}, {L"All files", L"*.*"}});
		for (auto& p : f) AddUnique(g_app.parts, p);
		AddPaths({});
	}
	RemoveClear("parts", g_app.parts, g_app.selectedPart);
	g_partsBand[1] = ImGui::GetCursorScreenPos().y;

	ImGui::SeparatorText("Animations");
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("Skyrim .hkx clips, or whole folders (searched recursively for .hkx; the output mirrors their subfolders). "
						"Optional: without clips only the model is written.");
	ImGui::PopTextWrapPos();
	PathList("inputs", g_app.inputs, g_app.selectedInput, 6);
	if (ImGui::Button("Add files...##inputs"))
		AddPaths(OpenFileDialog(hwnd, true, L"Add animation clips", {{L"Havok files", L"*.hkx"}, {L"All files", L"*.*"}}));
	ImGui::SameLine();
	if (ImGui::Button("Add folder...##inputs")) AddPaths(OpenFileDialog(hwnd, false, L"Add a folder of animations", {}, true));
	RemoveClear("inputs", g_app.inputs, g_app.selectedInput);

	ImGui::SeparatorText("Output");
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::Combo("##rig", &g_app.rig, kRigNames, 3);
	ImGui::SetNextItemWidth(-FLT_MIN);
	const std::string nameHint = g_app.parts.empty() ? "Name (default: the first part's file name)"
													 : "Name (default: " + fs::u8path(g_app.parts.front()).stem().u8string() + ")";
	ImGui::InputTextWithHint("##name", nameHint.c_str(), &g_app.name);
	PathField("##out", g_app.outDir, browseW);
	if (ImGui::Button("Browse...##out")) {
		auto f = OpenFileDialog(hwnd, false, L"Choose the output folder", {}, true);
		if (!f.empty()) g_app.outDir = f[0];
	}
	ImGui::TextDisabled("<name>.fbx + <name>.materials.json, and anims\\ with one .fbx + .anim.json per clip.");
	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::BeginDisabled(busy || g_app.skeletonNif.empty() || g_app.parts.empty() || g_app.outDir.empty());
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.25f, 1));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.65f, 0.30f, 1));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.45f, 0.20f, 1));
	if (ImGui::Button(busy ? "Working..." : "Go!", ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 2))) StartRun();
	const ImVec2 goMin = ImGui::GetItemRectMin(), goMax = ImGui::GetItemRectMax();
	g_goRect[0] = goMin.x, g_goRect[1] = goMin.y, g_goRect[2] = goMax.x, g_goRect[3] = goMax.y;
	ImGui::PopStyleColor(3);
	ImGui::EndDisabled();

	ImGui::SeparatorText("Log");
	if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
		std::lock_guard<std::mutex> lock(g_app.logMutex);
		for (auto& l : g_app.log) {
			ImVec4 col = l.level == nm::LogLevel::Error     ? ImVec4(1.0f, 0.45f, 0.4f, 1)
						 : l.level == nm::LogLevel::Warning ? ImVec4(1.0f, 0.8f, 0.35f, 1)
															: ImGui::GetStyleColorVec4(ImGuiCol_Text);
			ImGui::PushStyleColor(ImGuiCol_Text, col);
			ImGui::TextUnformatted(l.text.c_str());
			ImGui::PopStyleColor();
		}
		if (g_app.scrollLog) {
			ImGui::SetScrollHereY(1.0f);
			g_app.scrollLog = false;
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

} // namespace

int RunCreatureGui(const CreatureGuiInit& init) {
	AddPaths(init.inputs);
	for (auto& p : init.parts) AddUnique(g_app.parts, p);
	if (!init.skeletonNif.empty()) g_app.skeletonNif = init.skeletonNif;
	if (!init.skeletonHkx.empty()) g_app.skeletonHkx = init.skeletonHkx;
	if (!init.outDir.empty()) g_app.outDir = init.outDir;
	g_app.name = init.name;
	g_app.rig = init.rig == nm::CreatureRig::Humanoid ? 1 : init.rig == nm::CreatureRig::Generic ? 2 : 0;
	AddPaths({});
	bool autorun = init.go;
	bool closing = false;
	int exitCode = 0;

	GuiHostOptions host;
	host.className = L"SkyCreatureWindow";
	host.title = L"SkyCreature - Skyrim creature to FBX";
	host.width = 820;
	host.height = 1000;
	host.busy = [] { return g_app.running.load(); };
	host.drop = [](const std::vector<std::string>& files, POINT) { AddPaths(files); };
	host.frame = [&](HWND hwnd) {
		DrawUi(hwnd);
		if (!init.layoutFile.empty()) WriteLayout(init.layoutFile);
		if (autorun) {
			autorun = false;
			StartRun();
		}
		if (init.exitWhenDone && !closing && g_app.runsFinished > 0 && !g_app.running) {
			exitCode = g_app.lastRunFailed ? 1 : 0;
			closing = true;
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
		}
	};
	if (!RunGuiHost(host)) {
		MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"SkyCreature", MB_ICONERROR);
		return 1;
	}
	if (g_app.worker.joinable()) g_app.worker.join();
	return exitCode;
}
