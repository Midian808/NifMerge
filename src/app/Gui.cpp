// Dear ImGui front end (Win32 + Direct3D 11). One window: skeleton, parts, weight, output, Go!.
#include "Gui.hpp"

#include "core/Merge.hpp"
#include "core/Pipeline.hpp"
#include "core/TextUtil.hpp"

#include "GuiHost.hpp"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

// ---- App state ---------------------------------------------------------------------------------

struct LogLine {
	nm::LogLevel level;
	std::string text;
};

struct AppState {
	std::string skeleton;
	std::vector<std::string> partFiles; // as added; paired for display and for the run
	std::vector<std::string> outfitFiles; // outfit items, as added
	int selectedOutfit = -1;
	// What each outfit item does, recomputed when either list changes (see OutfitLines).
	std::vector<std::string> outfitKey;
	std::vector<std::string> outfitLines;
	std::string outfitError;
	std::vector<std::string> morphPaths; // .tri files and folders
	int selectedMorph = -1;
	int selectedPart = -1;
	float weight = 50.0f;
	std::string output;
	bool writeObj = false;

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

bool IsSkeletonFile(const std::string& path) {
	std::string n = fs::u8path(path).filename().u8string();
	for (auto& c : n) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return n.find("skeleton") != std::string::npos;
}

void AddMorphPath(const std::string& p) {
	for (auto& existing : g_app.morphPaths)
		if (fs::u8path(existing).lexically_normal() == fs::u8path(p).lexically_normal()) return;
	g_app.morphPaths.push_back(p);
}

// .nif files go to the skeleton or the parts, .tri files and folders to the facial morphs.
void AddFiles(const std::vector<std::string>& files) {
	for (const auto& f : files) {
		if (fs::is_directory(fs::u8path(f))) {
			AddMorphPath(f);
			continue;
		}
		std::string ext = fs::u8path(f).extension().u8string();
		for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		if (ext == ".tri") {
			AddMorphPath(f);
			continue;
		}
		if (ext != ".nif") {
			g_app.Log(nm::LogLevel::Warning, "Ignored (not a .nif or .tri): " + f);
			continue;
		}
		if (IsSkeletonFile(f)) g_app.skeleton = f;
		else g_app.partFiles.push_back(f);
	}
	if (g_app.output.empty() && !g_app.partFiles.empty())
		g_app.output = (fs::u8path(g_app.partFiles.front()).parent_path() / "Character.fbx").u8string();
}

void AddOutfitFiles(const std::vector<std::string>& files) {
	for (const auto& f : files) {
		std::string ext = fs::u8path(f).extension().u8string();
		for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		if (ext == ".nif") g_app.outfitFiles.push_back(f);
		else g_app.Log(nm::LogLevel::Warning, "Ignored (not a .nif): " + f);
	}
}

// The list sections files can be dropped on. Each is a horizontal band of the window, from its title
// to the next section's title, recorded while drawing (client pixels).
enum class Section { Parts, Outfit, Morphs };

struct SectionBand {
	Section section;
	float top, bottom;
};

std::vector<SectionBand> g_sections;
float g_goRect[4] = {}; // Go! button: x0, y0, x1, y1

// Files dropped on a section go to that list; dropped anywhere else, they are sorted by type.
void AddDroppedFiles(const std::vector<std::string>& files, POINT at) {
	const SectionBand* band = nullptr;
	for (const auto& b : g_sections)
		if (at.y >= b.top && at.y < b.bottom) band = &b;
	if (!band || band->section == Section::Parts) {
		AddFiles(files);
		return;
	}
	if (band->section == Section::Outfit) {
		AddOutfitFiles(files);
		return;
	}
	for (const auto& f : files) {
		std::string ext = fs::u8path(f).extension().u8string();
		for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		if (ext == ".tri" || fs::is_directory(fs::u8path(f))) AddMorphPath(f);
		else g_app.Log(nm::LogLevel::Warning, "Ignored (not a .tri file or a folder): " + f);
	}
}

// For the GUI test: writes the drop sections and the Go! button, in client pixels, whenever they move.
void WriteLayout(const std::string& path) {
	static std::string written;
	auto f = [](float v) { return std::to_string(static_cast<int>(v)); };
	std::string json = "{";
	for (const auto& b : g_sections) {
		const char* name = b.section == Section::Parts ? "parts" : b.section == Section::Outfit ? "outfit" : "morphs";
		json += std::string("\"") + name + "\": [" + f(b.top) + ", " + f(b.bottom) + "], ";
	}
	json += "\"go\": [" + f(g_goRect[0]) + ", " + f(g_goRect[1]) + ", " + f(g_goRect[2]) + ", " + f(g_goRect[3]) + "]}\n";
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

// One line per outfit item saying what it replaces, read from the files' body slots. Recomputed only
// when the parts or the outfit change.
void UpdateOutfitLines() {
	std::vector<std::string> key = g_app.partFiles;
	key.push_back("|");
	key.insert(key.end(), g_app.outfitFiles.begin(), g_app.outfitFiles.end());
	if (key == g_app.outfitKey) return;
	g_app.outfitKey = key;
	g_app.outfitLines.clear();
	g_app.outfitError.clear();
	try {
		g_app.outfitLines = nm::DescribeOutfit(nm::PairPartFiles(g_app.partFiles), nm::PairPartFiles(g_app.outfitFiles));
	}
	catch (const std::exception& e) {
		g_app.outfitError = e.what();
	}
}

// ---- File dialogs ------------------------------------------------------------------------------

enum class Pick { Nif, Tri, Folder };

std::vector<std::string> OpenFiles(HWND owner, bool multi, const wchar_t* title, Pick pick = Pick::Nif) {
	if (pick == Pick::Folder) return OpenFileDialog(owner, multi, title, {}, true);
	if (pick == Pick::Tri) return OpenFileDialog(owner, multi, title, {{L"Morph files", L"*.tri"}, {L"All files", L"*.*"}});
	return OpenFileDialog(owner, multi, title, {{L"NIF files", L"*.nif"}, {L"All files", L"*.*"}});
}

std::string SaveFile(HWND owner, const std::string& current) {
	return SaveFileDialog(owner, current, L"Save merged model as", L"FBX files", L"*.fbx", L"fbx");
}

// ---- Run ---------------------------------------------------------------------------------------

void StartRun() {
	if (g_app.running) return;
	if (g_app.worker.joinable()) g_app.worker.join();
	nm::RunOptions opt;
	opt.request.skeletonPath = g_app.skeleton;
	opt.request.parts = nm::PairPartFiles(g_app.partFiles);
	opt.request.outfit = nm::PairPartFiles(g_app.outfitFiles);
	opt.request.weight = g_app.weight;
	opt.request.morphPaths = g_app.morphPaths;
	opt.fbxPath = g_app.output;
	if (g_app.writeObj && !opt.fbxPath.empty())
		opt.objPath = (fs::u8path(opt.fbxPath).replace_extension(".obj")).u8string();
	g_app.Log(nm::LogLevel::Info, "---- Go!");
	g_app.running = true;
	g_app.worker = std::thread([opt] {
		bool failed = false;
		try {
			nm::RunResult r = nm::RunPipeline(opt, [](nm::LogLevel l, const std::string& s) { g_app.Log(l, s); });
			g_app.Log(nm::LogLevel::Info, "Done: " + std::to_string(r.materials) + " material slots, " +
											  std::to_string(r.vertices) + " vertices, " + std::to_string(r.bones) +
											  " bones.");
		}
		catch (const std::exception& e) {
			g_app.Log(nm::LogLevel::Error, e.what());
			failed = true;
		}
		g_app.lastRunFailed = failed;
		g_app.runsFinished++;
		g_app.running = false;
	});
}

// ---- UI ----------------------------------------------------------------------------------------

void DrawUi(HWND hwnd) {
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::Begin("NifMerge", nullptr,
				 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	const bool busy = g_app.running;
	const float browseW = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
	ImGui::BeginDisabled(busy);

	ImGui::SeparatorText("Skeleton");
	ImGui::SetNextItemWidth(-browseW - ImGui::GetStyle().ItemSpacing.x);
	ImGui::InputText("##skeleton", g_app.skeleton.data(), g_app.skeleton.size() + 1, ImGuiInputTextFlags_ReadOnly);
	ImGui::SameLine();
	if (ImGui::Button("Browse...##skel")) {
		auto f = OpenFiles(hwnd, false, L"Choose the skeleton nif");
		if (!f.empty()) g_app.skeleton = f[0];
	}

	const float partsTop = ImGui::GetCursorScreenPos().y;
	ImGui::SeparatorText("Model parts");
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("Body, hands, feet, head / FaceGen head, hair... _0/_1 weight files pair automatically. "
						"You can also drag files onto this window: onto a list to add them to it, anywhere else "
						"to sort them (.nif to the parts, .tri files and folders to the facial morphs).");
	ImGui::PopTextWrapPos();
	auto parts = nm::PairPartFiles(g_app.partFiles);
	const float listH = ImGui::GetTextLineHeightWithSpacing() * 8;
	if (ImGui::BeginListBox("##parts", ImVec2(-FLT_MIN, listH))) {
		for (int i = 0; i < static_cast<int>(parts.size()); i++) {
			std::string label = fs::u8path(parts[i].path0).filename().u8string();
			if (parts[i].path1) label += "  +  " + fs::u8path(*parts[i].path1).filename().u8string() + "   (weight pair)";
			label += "##" + std::to_string(i);
			if (ImGui::Selectable(label.c_str(), g_app.selectedPart == i)) g_app.selectedPart = i;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", parts[i].path0.c_str());
		}
		ImGui::EndListBox();
	}
	if (ImGui::Button("Add files...")) AddFiles(OpenFiles(hwnd, true, L"Add model part nifs"));
	ImGui::SameLine();
	ImGui::BeginDisabled(g_app.selectedPart < 0 || g_app.selectedPart >= static_cast<int>(parts.size()));
	if (ImGui::Button("Remove")) {
		const auto& p = parts[g_app.selectedPart];
		auto same = [](const std::string& a, const std::string& b) {
			return fs::u8path(a).lexically_normal() == fs::u8path(b).lexically_normal();
		};
		auto& files = g_app.partFiles;
		files.erase(std::remove_if(files.begin(), files.end(),
								   [&](const std::string& f) { return same(f, p.path0) || (p.path1 && same(f, *p.path1)); }),
					files.end());
		g_app.selectedPart = -1;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Clear")) {
		g_app.partFiles.clear();
		g_app.selectedPart = -1;
	}

	const float outfitTop = ImGui::GetCursorScreenPos().y;
	ImGui::SeparatorText("Outfit (optional)");
	ImGui::TextDisabled("Clothing and armour the character wears. Each item replaces the character's parts in the body "
						"slots it takes (a dress replaces the body, boots the feet); other items are added on top.");
	UpdateOutfitLines();
	auto outfit = nm::PairPartFiles(g_app.outfitFiles);
	if (ImGui::BeginListBox("##outfit", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 5))) {
		for (int i = 0; i < static_cast<int>(outfit.size()); i++) {
			std::string label = fs::u8path(outfit[i].path0).filename().u8string();
			if (outfit[i].path1) label += "  +  " + fs::u8path(*outfit[i].path1).filename().u8string() + "   (weight pair)";
			label += "##o" + std::to_string(i);
			if (ImGui::Selectable(label.c_str(), g_app.selectedOutfit == i)) g_app.selectedOutfit = i;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", outfit[i].path0.c_str());
			if (i < static_cast<int>(g_app.outfitLines.size())) { // "Outfit: <files> takes slot N: replaces ..."
				const std::string& line = g_app.outfitLines[i];
				const std::string prefix = "Outfit: ";
				ImGui::TextDisabled("      %s", line.rfind(prefix, 0) == 0 ? line.c_str() + prefix.size() : line.c_str());
			}
		}
		ImGui::EndListBox();
	}
	if (!g_app.outfitError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", g_app.outfitError.c_str());
	if (ImGui::Button("Add files...##outfit")) AddOutfitFiles(OpenFiles(hwnd, true, L"Add outfit item nifs"));
	ImGui::SameLine();
	ImGui::BeginDisabled(g_app.selectedOutfit < 0 || g_app.selectedOutfit >= static_cast<int>(outfit.size()));
	if (ImGui::Button("Remove##outfit")) {
		const auto& p = outfit[g_app.selectedOutfit];
		auto same = [](const std::string& a, const std::string& b) {
			return fs::u8path(a).lexically_normal() == fs::u8path(b).lexically_normal();
		};
		auto& files = g_app.outfitFiles;
		files.erase(std::remove_if(files.begin(), files.end(),
								   [&](const std::string& f) { return same(f, p.path0) || (p.path1 && same(f, *p.path1)); }),
					files.end());
		g_app.selectedOutfit = -1;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Clear##outfit")) {
		g_app.outfitFiles.clear();
		g_app.selectedOutfit = -1;
	}

	const float morphsTop = ImGui::GetCursorScreenPos().y;
	ImGui::SeparatorText("Facial morphs (.tri, optional)");
	ImGui::TextDisabled("Expression, phoneme, blink and eye-look morphs become blend shapes. Add .tri files or whole "
						"folders: each file is matched to a head part by its geometry, and the log shows the decisions.");
	if (ImGui::BeginListBox("##morphs", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 4))) {
		for (int i = 0; i < static_cast<int>(g_app.morphPaths.size()); i++) {
			const std::string& p = g_app.morphPaths[i];
			std::string label = (fs::is_directory(fs::u8path(p)) ? "[folder] " : "") + p + "##m" + std::to_string(i);
			if (ImGui::Selectable(label.c_str(), g_app.selectedMorph == i)) g_app.selectedMorph = i;
		}
		ImGui::EndListBox();
	}
	if (ImGui::Button("Add files...##tri"))
		for (auto& f : OpenFiles(hwnd, true, L"Add morph (.tri) files", Pick::Tri)) AddMorphPath(f);
	ImGui::SameLine();
	if (ImGui::Button("Add folder...##tri"))
		for (auto& f : OpenFiles(hwnd, false, L"Add a folder of morph (.tri) files", Pick::Folder)) AddMorphPath(f);
	ImGui::SameLine();
	ImGui::BeginDisabled(g_app.selectedMorph < 0 || g_app.selectedMorph >= static_cast<int>(g_app.morphPaths.size()));
	if (ImGui::Button("Remove##tri")) {
		g_app.morphPaths.erase(g_app.morphPaths.begin() + g_app.selectedMorph);
		g_app.selectedMorph = -1;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Clear##tri")) {
		g_app.morphPaths.clear();
		g_app.selectedMorph = -1;
	}

	const float weightTop = ImGui::GetCursorScreenPos().y;
	g_sections = {{Section::Parts, partsTop, outfitTop},
				  {Section::Outfit, outfitTop, morphsTop},
				  {Section::Morphs, morphsTop, weightTop}};

	ImGui::SeparatorText("Body weight");
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::SliderFloat("##weight", &g_app.weight, 0.0f, 100.0f, "%.0f");
	ImGui::TextDisabled("Blends each _0 / _1 pair, as the game's NPC weight does. 0 = thin (_0), 100 = heavy (_1).");

	ImGui::SeparatorText("Output");
	ImGui::SetNextItemWidth(-browseW - ImGui::GetStyle().ItemSpacing.x);
	ImGui::InputText("##output", g_app.output.data(), g_app.output.size() + 1, ImGuiInputTextFlags_ReadOnly);
	ImGui::SameLine();
	if (ImGui::Button("Browse...##out")) {
		std::string f = SaveFile(hwnd, g_app.output);
		if (!f.empty()) g_app.output = f;
	}
	ImGui::Checkbox("Also write a debug .obj (rest pose, Skyrim space)", &g_app.writeObj);
	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::BeginDisabled(busy || g_app.skeleton.empty() || g_app.partFiles.empty() || g_app.output.empty());
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

int RunGui(const GuiInit& init) {
	AddFiles(init.files);
	AddOutfitFiles(init.outfit);
	for (auto& p : init.morphPaths) AddMorphPath(p);
	if (!init.skeleton.empty()) g_app.skeleton = init.skeleton;
	if (!init.output.empty()) g_app.output = init.output;
	g_app.weight = init.weight;
	bool autorun = init.go;
	bool closing = false;
	int exitCode = 0;

	GuiHostOptions host;
	host.className = L"NifMergeWindow";
	host.title = L"NifMerge - Skyrim NIF to Unity Humanoid FBX";
	host.width = 820; // tall enough for every section, Go! and some log
	host.height = 1200;
	host.busy = [] { return g_app.running.load(); };
	host.drop = [](const std::vector<std::string>& files, POINT at) { AddDroppedFiles(files, at); };
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
		MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"NifMerge", MB_ICONERROR);
		return 1;
	}
	if (g_app.worker.joinable()) g_app.worker.join();
	return exitCode;
}
