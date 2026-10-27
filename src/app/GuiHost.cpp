#include "GuiHost.hpp"

#include "core/TextUtil.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

namespace fs = std::filesystem;

// ---- Direct3D plumbing -------------------------------------------------------------------------

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resizeW = 0, g_resizeH = 0;
const GuiHostOptions* g_opt = nullptr;

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
	g_swapChain = nullptr;
	g_context = nullptr;
	g_device = nullptr;
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
			if (g_opt && g_opt->drop && !(g_opt->busy && g_opt->busy())) {
				POINT at{};
				DragQueryPoint(drop, &at);
				UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
				std::vector<std::string> files;
				for (UINT i = 0; i < n; i++) {
					wchar_t buf[MAX_PATH * 4];
					if (DragQueryFileW(drop, i, buf, static_cast<UINT>(std::size(buf)))) files.push_back(nm::ToUtf8(buf));
				}
				g_opt->drop(files, at);
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

bool RunGuiHost(const GuiHostOptions& opt) {
	g_opt = &opt;
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	ImGui_ImplWin32_EnableDpiAwareness();
	const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

	WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr,
					  nullptr, opt.className, nullptr};
	wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	RegisterClassExW(&wc);
	// Shrunk and moved up to fit the screen above the taskbar.
	MONITORINFO mon = {sizeof(mon)};
	GetMonitorInfoW(MonitorFromPoint(POINT{100, 100}, MONITOR_DEFAULTTOPRIMARY), &mon);
	const RECT& work = mon.rcWork;
	const int winW = std::min(static_cast<int>(opt.width * dpi), static_cast<int>(work.right - work.left));
	const int winH = std::min(static_cast<int>(opt.height * dpi), static_cast<int>(work.bottom - work.top));
	const int winX = std::max(static_cast<int>(work.left), std::min(static_cast<int>(work.left) + 100, static_cast<int>(work.right) - winW));
	const int winY = std::max(static_cast<int>(work.top), std::min(static_cast<int>(work.top) + 100, static_cast<int>(work.bottom) - winH));
	HWND hwnd = CreateWindowW(wc.lpszClassName, opt.title, WS_OVERLAPPEDWINDOW, winX, winY, winW, winH, nullptr, nullptr,
							  wc.hInstance, nullptr);
	if (!CreateDevice(hwnd)) {
		CleanupDevice();
		DestroyWindow(hwnd);
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		CoUninitialize();
		g_opt = nullptr;
		return false;
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
		opt.frame(hwnd);
		ImGui::Render();
		const float clear[4] = {0.1f, 0.1f, 0.11f, 1.0f};
		g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_context->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_swapChain->Present(1, 0);
	}

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	CleanupDevice();
	if (IsWindow(hwnd)) DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
	CoUninitialize();
	g_opt = nullptr;
	return true;
}

std::vector<std::string> OpenFileDialog(HWND owner, bool multi, const wchar_t* title,
										const std::vector<std::pair<const wchar_t*, const wchar_t*>>& filters, bool folders) {
	std::vector<std::string> out;
	IFileOpenDialog* dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
	std::vector<COMDLG_FILTERSPEC> specs;
	for (auto& [name, pattern] : filters) specs.push_back({name, pattern});
	if (!folders && !specs.empty()) dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
	dlg->SetTitle(title);
	DWORD opts = 0;
	dlg->GetOptions(&opts);
	dlg->SetOptions(opts | FOS_FILEMUSTEXIST | (multi ? FOS_ALLOWMULTISELECT : 0) | (folders ? FOS_PICKFOLDERS : 0));
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

std::string SaveFileDialog(HWND owner, const std::string& current, const wchar_t* title, const wchar_t* filterName,
						   const wchar_t* pattern, const wchar_t* extension) {
	std::string out;
	IFileSaveDialog* dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
	COMDLG_FILTERSPEC filters[] = {{filterName, pattern}};
	dlg->SetFileTypes(1, filters);
	dlg->SetDefaultExtension(extension);
	dlg->SetTitle(title);
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
