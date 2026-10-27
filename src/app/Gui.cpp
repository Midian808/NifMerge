// Dear ImGui front end (Win32 + Direct3D 11). One window: skeleton, parts, weight, output, Go!.
#include "Gui.hpp"

#include "core/Merge.hpp"
#include "core/Pipeline.hpp"
#include "core/TextUtil.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

namespace fs = std::filesystem;

// ---- Direct3D plumbing -------------------------------------------------------------------------

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resizeW = 0, g_resizeH = 0;

void CreateRenderTarget() {
	ID3D11Texture2D* back = nullptr;
	g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
	g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
	back->Release();
}

void CleanupRenderTarget() {
	if (g_rtv) {
		g_rtv->Release();
		g_rtv = nullptr;
	}
}

bool CreateDevice(HWND hwnd) {
	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
	D3D_FEATURE_LEVEL got;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
											   D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
	if (hr == DXGI_ERROR_UNSUPPORTED) // no GPU driver: fall back to software rendering
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
										   &g_swapChain, &g_device, &got, &g_context);
	if (FAILED(hr)) return false;
	CreateRenderTarget();
	return true;
}

void CleanupDevice() {
	CleanupRenderTarget();
	if (g_swapChain) g_swapChain->Release();
	if (g_context) g_context->Release();
	if (g_device) g_device->Release();
}

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
	std::vector<std::string> out;
	IFileOpenDialog* dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
	COMDLG_FILTERSPEC nif[] = {{L"NIF files", L"*.nif"}, {L"All files", L"*.*"}};
	COMDLG_FILTERSPEC tri[] = {{L"Morph files", L"*.tri"}, {L"All files", L"*.*"}};
	if (pick != Pick::Folder) dlg->SetFileTypes(2, pick == Pick::Tri ? tri : nif);
	dlg->SetTitle(title);
	DWORD opts = 0;
	dlg->GetOptions(&opts);
	dlg->SetOptions(opts | FOS_FILEMUSTEXIST | (multi ? FOS_ALLOWMULTISELECT : 0) |
					(pick == Pick::Folder ? FOS_PICKFOLDERS : 0));
	if (SUCCEEDED(dlg->Show(owner))) {
		IShellItemArray* items = nullptr;
		if (SUCCEEDED(dlg->GetResults(&items))) {
			DWORD count = 0;
			items->GetCount(&count);
			for (DWORD i = 0; i < count; i++) {
				IShellItem* item = nullptr;
				PWSTR path = nullptr;
				if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
					out.push_back(nm::ToUtf8(path));
					CoTaskMemFree(path);
				}
				if (item) item->Release();
			}
			items->Release();
		}
	}
	dlg->Release();
	return out;
}

std::string SaveFile(HWND owner, const std::string& current) {
	std::string out;
	IFileSaveDialog* dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
	COMDLG_FILTERSPEC filters[] = {{L"FBX files", L"*.fbx"}};
	dlg->SetFileTypes(1, filters);
	dlg->SetDefaultExtension(L"fbx");
	dlg->SetTitle(L"Save merged model as");
	if (!current.empty()) dlg->SetFileName(fs::u8path(current).filename().wstring().c_str());
	if (SUCCEEDED(dlg->Show(owner))) {
		IShellItem* item = nullptr;
		PWSTR path = nullptr;
		if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
			out = nm::ToUtf8(path);
			CoTaskMemFree(path);
		}
		if (item) item->Release();
	}
	dlg->Release();
	return out;
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

	ImGui::SeparatorText("Model parts");
	ImGui::TextDisabled("Body, hands, feet, head / FaceGen head, hair... _0/_1 weight files pair automatically. "
						"You can also drag .nif and .tri files, or folders of .tri files, onto this window.");
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

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
	switch (msg) {
		case WM_SIZE:
			if (wParam != SIZE_MINIMIZED) {
				g_resizeW = LOWORD(lParam);
				g_resizeH = HIWORD(lParam);
			}
			return 0;
		case WM_DROPFILES: {
			HDROP drop = reinterpret_cast<HDROP>(wParam);
			if (!g_app.running) {
				UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
				std::vector<std::string> files;
				for (UINT i = 0; i < n; i++) {
					wchar_t buf[MAX_PATH * 4];
					if (DragQueryFileW(drop, i, buf, static_cast<UINT>(std::size(buf)))) files.push_back(nm::ToUtf8(buf));
				}
				AddFiles(files);
			}
			DragFinish(drop);
			return 0;
		}
		case WM_SYSCOMMAND:
			if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
			break;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

int RunGui(const GuiInit& init) {
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	ImGui_ImplWin32_EnableDpiAwareness();
	const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

	WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr,
					  nullptr, L"NifMergeWindow", nullptr};
	wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	RegisterClassExW(&wc);
	HWND hwnd = CreateWindowW(wc.lpszClassName, L"NifMerge - Skyrim NIF to Unity Humanoid FBX", WS_OVERLAPPEDWINDOW,
							  100, 100, static_cast<int>(820 * dpi), static_cast<int>(900 * dpi), nullptr, nullptr,
							  wc.hInstance, nullptr);
	if (!CreateDevice(hwnd)) {
		CleanupDevice();
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"NifMerge", MB_ICONERROR);
		return 1;
	}
	DragAcceptFiles(hwnd, TRUE);
	ShowWindow(hwnd, SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGui::GetStyle().ScaleAllSizes(dpi);
	const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
	if (fs::exists(segoe)) io.Fonts->AddFontFromFileTTF(segoe, 17.0f * dpi);
	else io.FontGlobalScale = dpi;
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_device, g_context);

	AddFiles(init.files);
	AddOutfitFiles(init.outfit);
	for (auto& p : init.morphPaths) AddMorphPath(p);
	if (!init.skeleton.empty()) g_app.skeleton = init.skeleton;
	if (!init.output.empty()) g_app.output = init.output;
	g_app.weight = init.weight;
	bool autorun = init.go;
	bool closing = false;
	int exitCode = 0;

	bool done = false;
	while (!done) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT) done = true;
		}
		if (done) break;
		if (g_resizeW && g_resizeH) {
			CleanupRenderTarget();
			g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
			g_resizeW = g_resizeH = 0;
			CreateRenderTarget();
		}
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		DrawUi(hwnd);
		if (autorun) {
			autorun = false;
			StartRun();
		}
		if (init.exitWhenDone && !closing && g_app.runsFinished > 0 && !g_app.running) {
			exitCode = g_app.lastRunFailed ? 1 : 0;
			closing = true;
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
		}
		ImGui::Render();
		const float clear[4] = {0.1f, 0.1f, 0.11f, 1.0f};
		g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_context->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_swapChain->Present(1, 0);
	}

	if (g_app.worker.joinable()) g_app.worker.join();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	CleanupDevice();
	if (IsWindow(hwnd)) DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
	CoUninitialize();
	return exitCode;
}
