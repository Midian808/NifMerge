// SkyAnim's Dear ImGui front end: skeleton (.nif + .hkx), clips (files and folders), output folder, Go!.
#include "AnimGui.hpp"

#include "anim/Batch.hpp"
#include "anim/HkxAnimation.hpp"
#include "app/GuiHost.hpp"

#include "imgui.h"

#include <algorithm>
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
	std::string skeletonNif, skeletonHkx, outDir;
	std::vector<std::string> inputs; // clips and folders
	int selected = -1;

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
float g_dropBand[2] = {}; // inputs section: top, bottom (client pixels)
float g_goRect[4] = {};

std::string LowerExt(const std::string& path) {
	std::string e = fs::u8path(path).extension().u8string();
	for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return e;
}

void AddInput(const std::string& p) {
	for (auto& existing : g_app.inputs)
		if (fs::u8path(existing).lexically_normal() == fs::u8path(p).lexically_normal()) return;
	g_app.inputs.push_back(p);
}

// Dropped or picked files and folders: a .nif fills the skeleton, a .hkx holding a skeleton fills the
// Havok skeleton, folders and clips join the inputs. Two skeletons of one kind in one drop fill
// neither: the user picks one.
void AddPaths(const std::vector<std::string>& paths) {
	std::vector<std::string> nifs, hkxSkeletons;
	for (auto& p : paths) {
		if (fs::is_directory(fs::u8path(p))) {
			AddInput(p);
			continue;
		}
		const std::string ext = LowerExt(p);
		if (ext == ".nif") {
			nifs.push_back(p);
		} else if (ext == ".hkx") {
			nm::HkxKind kind = nm::HkxKind::Animation; // an unreadable file fails as a clip, with its reason
			try {
				kind = nm::Classify(nm::HkxPackfile::Load(p));
			} catch (const std::exception&) {
			}
			if (kind == nm::HkxKind::Skeleton) hkxSkeletons.push_back(p);
			else if (kind == nm::HkxKind::Animation) AddInput(p);
			else g_app.Log(nm::LogLevel::Info, "Skipped (holds no animation or skeleton): " + p);
		} else {
			g_app.Log(nm::LogLevel::Warning, "Ignored (not a .hkx, .nif or folder): " + p);
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
	if (g_app.outDir.empty() && !g_app.inputs.empty()) {
		fs::path first = fs::u8path(g_app.inputs.front());
		g_app.outDir = ((fs::is_directory(first) ? first : first.parent_path()) / "fbx").u8string();
	}
}

void WriteLayout(const std::string& path) {
	static std::string written;
	auto f = [](float v) { return std::to_string(static_cast<int>(v)); };
	std::string json = "{\"inputs\": [" + f(g_dropBand[0]) + ", " + f(g_dropBand[1]) + "], \"go\": [" + f(g_goRect[0]) +
					   ", " + f(g_goRect[1]) + ", " + f(g_goRect[2]) + ", " + f(g_goRect[3]) + "]}\n";
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
	nm::BatchRequest req;
	req.inputs = g_app.inputs;
	req.skeletonNif = g_app.skeletonNif;
	req.skeletonHkx = g_app.skeletonHkx;
	req.outDir = g_app.outDir;
	g_app.Log(nm::LogLevel::Info, "---- Go!");
	g_app.running = true;
	g_app.worker = std::thread([req] {
		bool failed = false;
		try {
			nm::BatchResult r = nm::RunBatch(req, [](nm::LogLevel l, const std::string& s) { g_app.Log(l, s); });
			failed = r.failed() > 0;
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

void DrawUi(HWND hwnd) {
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::Begin("SkyAnim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	const bool busy = g_app.running;
	const float browseW = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
	ImGui::BeginDisabled(busy);

	ImGui::SeparatorText("Skeleton");
	ImGui::TextDisabled("The character's skeleton.nif and its Havok skeleton (.hkx, found beside the .nif when it is there).");
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

	g_dropBand[0] = ImGui::GetCursorScreenPos().y;
	ImGui::SeparatorText("Animations");
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("Skyrim LE or SE .hkx clips, or whole folders (searched recursively; the output mirrors their "
						"subfolders). Drag files and folders onto this window: skeletons are recognised by content.");
	ImGui::PopTextWrapPos();
	if (ImGui::BeginListBox("##inputs", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 10))) {
		for (int i = 0; i < static_cast<int>(g_app.inputs.size()); i++) {
			const std::string& p = g_app.inputs[i];
			std::string label = (fs::is_directory(fs::u8path(p)) ? "[folder] " : "") + p + "##i" + std::to_string(i);
			if (ImGui::Selectable(label.c_str(), g_app.selected == i)) g_app.selected = i;
		}
		ImGui::EndListBox();
	}
	if (ImGui::Button("Add files...")) AddPaths(OpenFileDialog(hwnd, true, L"Add animation clips", {{L"Havok files", L"*.hkx"}, {L"All files", L"*.*"}}));
	ImGui::SameLine();
	if (ImGui::Button("Add folder...")) AddPaths(OpenFileDialog(hwnd, false, L"Add a folder of animations", {}, true));
	ImGui::SameLine();
	ImGui::BeginDisabled(g_app.selected < 0 || g_app.selected >= static_cast<int>(g_app.inputs.size()));
	if (ImGui::Button("Remove")) {
		g_app.inputs.erase(g_app.inputs.begin() + g_app.selected);
		g_app.selected = -1;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Clear")) {
		g_app.inputs.clear();
		g_app.selected = -1;
	}
	g_dropBand[1] = ImGui::GetCursorScreenPos().y;

	ImGui::SeparatorText("Output folder");
	PathField("##out", g_app.outDir, browseW);
	if (ImGui::Button("Browse...##out")) {
		auto f = OpenFileDialog(hwnd, false, L"Choose the output folder", {}, true);
		if (!f.empty()) g_app.outDir = f[0];
	}
	ImGui::TextDisabled("One .fbx and one .anim.json per clip, plus batch_report.json.");
	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::BeginDisabled(busy || g_app.skeletonNif.empty() || g_app.inputs.empty() || g_app.outDir.empty());
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

int RunAnimGui(const AnimGuiInit& init) {
	AddPaths(init.inputs);
	if (!init.skeletonNif.empty()) g_app.skeletonNif = init.skeletonNif;
	if (!init.skeletonHkx.empty()) g_app.skeletonHkx = init.skeletonHkx;
	if (!init.outDir.empty()) g_app.outDir = init.outDir;
	bool autorun = init.go;
	bool closing = false;
	int exitCode = 0;

	GuiHostOptions host;
	host.className = L"SkyAnimWindow";
	host.title = L"SkyAnim - Skyrim HKX animation to FBX";
	host.width = 820;
	host.height = 900;
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
		MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"SkyAnim", MB_ICONERROR);
		return 1;
	}
	if (g_app.worker.joinable()) g_app.worker.join();
	return exitCode;
}
