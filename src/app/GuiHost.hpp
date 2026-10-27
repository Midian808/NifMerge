#pragma once
// The Win32 + Direct3D 11 + Dear ImGui window both GUIs run in: DPI-aware window sized to the work
// area, file drops, native file dialogs, and the render loop.
#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

struct GuiHostOptions {
	const wchar_t* className = L"ImGuiHostWindow";
	const wchar_t* title = L"";
	int width = 820, height = 1200; // at 96 dpi; shrunk to fit the work area
	// Called every frame between ImGui::NewFrame and ImGui::Render: draw the UI and run per-frame logic.
	std::function<void(HWND)> frame;
	// Files dropped on the window (paths and the client point); ignored while busy() is true.
	std::function<void(const std::vector<std::string>&, POINT)> drop;
	std::function<bool()> busy;
};

// Runs until the window closes. Returns false if no Direct3D 11 device could be created.
bool RunGuiHost(const GuiHostOptions& opt);

// Native open dialog. filters: (description, pattern) pairs; folders picks folders instead of files.
std::vector<std::string> OpenFileDialog(HWND owner, bool multi, const wchar_t* title,
										const std::vector<std::pair<const wchar_t*, const wchar_t*>>& filters, bool folders = false);
// Native save dialog for one file with the given extension (without the dot); empty when cancelled.
std::string SaveFileDialog(HWND owner, const std::string& current, const wchar_t* title, const wchar_t* filterName,
						   const wchar_t* pattern, const wchar_t* extension);
