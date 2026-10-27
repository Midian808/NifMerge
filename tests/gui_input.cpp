// Real mouse input for the GUI test.
//   gui_input drop <hwnd> <x> <y> <file>...   drags the files onto the window and drops them at the client point,
//                                             through OLE drag and drop, as Explorer does: the left button is
//                                             pressed in a small source window, moved to the point and released
//   gui_input click <hwnd> <x> <y>            left-clicks the client point with the real mouse
#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

// A data object offering the files as CF_HDROP, the format Explorer drags files in.
class FileData : public IDataObject {
public:
	explicit FileData(const std::vector<std::wstring>& files) {
		size_t chars = 1;
		for (const auto& f : files) chars += f.size() + 1;
		const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
		drop_ = GlobalAlloc(GHND, bytes);
		auto* df = static_cast<DROPFILES*>(GlobalLock(drop_));
		df->pFiles = sizeof(DROPFILES); // pt stays 0,0: the drop point is filled in by the system
		df->fWide = TRUE;
		auto* p = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
		for (const auto& f : files) {
			wcscpy_s(p, f.size() + 1, f.c_str());
			p += f.size() + 1;
		}
		*p = 0;
		GlobalUnlock(drop_);
	}
	~FileData() { GlobalFree(drop_); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
		if (riid == IID_IUnknown || riid == IID_IDataObject) {
			*out = this;
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
	ULONG STDMETHODCALLTYPE Release() override { return --refs_; } // lives on the stack of the drag

	HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fmt, STGMEDIUM* medium) override {
		if (QueryGetData(fmt) != S_OK) return DV_E_FORMATETC;
		const SIZE_T size = GlobalSize(drop_);
		HGLOBAL copy = GlobalAlloc(GHND, size);
		memcpy(GlobalLock(copy), GlobalLock(drop_), size);
		GlobalUnlock(copy);
		GlobalUnlock(drop_);
		medium->tymed = TYMED_HGLOBAL;
		medium->hGlobal = copy;
		medium->pUnkForRelease = nullptr;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fmt) override {
		return fmt->cfFormat == CF_HDROP && (fmt->tymed & TYMED_HGLOBAL) && fmt->dwAspect == DVASPECT_CONTENT
				   ? S_OK
				   : DV_E_FORMATETC;
	}
	HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
		out->ptd = nullptr;
		return E_NOTIMPL;
	}
	HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
		if (dir != DATADIR_GET) return E_NOTIMPL;
		FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
		return SHCreateStdEnumFmtEtc(1, &fmt, out);
	}
	HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
	HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
	HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
	HGLOBAL drop_ = nullptr;
	ULONG refs_ = 1;
};

// The usual drop source: drops when the left button is released, cancels on Escape.
class DropSource : public IDropSource {
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
		if (riid == IID_IUnknown || riid == IID_IDropSource) {
			*out = this;
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
	ULONG STDMETHODCALLTYPE Release() override { return --refs_; }

	HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
		if (escape) return DRAGDROP_S_CANCEL;
		return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
	}
	HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
	ULONG refs_ = 1;
};

void SendButton(DWORD flag) {
	INPUT in = {};
	in.type = INPUT_MOUSE;
	in.mi.dwFlags = flag;
	SendInput(1, &in, sizeof(INPUT));
}

// Brings the window to the top so it is the one under the cursor, and returns the client point on screen.
POINT ScreenPoint(HWND hwnd, int x, int y) {
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
	SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
	SetForegroundWindow(hwnd);
	POINT p{x, y};
	ClientToScreen(hwnd, &p);
	return p;
}

// The drag in progress, started from the source window's WM_LBUTTONDOWN.
struct Drag {
	const std::vector<std::wstring>* files = nullptr;
	std::atomic<bool> started{false};
	bool finished = false;
	HRESULT hr = E_FAIL;
	DWORD effect = DROPEFFECT_NONE;
};
Drag g_drag;

LRESULT CALLBACK SourceProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_LBUTTONDOWN && !g_drag.started) {
		g_drag.started = true;
		FileData data(*g_drag.files);
		DropSource source;
		g_drag.hr = DoDragDrop(&data, &source, DROPEFFECT_COPY | DROPEFFECT_LINK, &g_drag.effect);
		g_drag.finished = true;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Presses the button in a small window of this process placed at `from`, moves to `to` and releases.
int DragFiles(const std::vector<std::wstring>& files, POINT from, POINT to) {
	OleInitialize(nullptr);
	WNDCLASSW wc = {};
	wc.lpfnWndProc = SourceProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
	wc.lpszClassName = L"GuiInputDragSource";
	RegisterClassW(&wc);
	const int size = 24;
	HWND source = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
								  from.x - size / 2, from.y - size / 2, size, size, nullptr, nullptr, wc.hInstance, nullptr);
	UpdateWindow(source);
	SetCursorPos(from.x, from.y);
	Sleep(100);
	g_drag.files = &files;

	// Moves the cursor once the drag has started (in steps, as a hand would), lingers over the target so the
	// drag loop enters it, then releases the button. Releases it anyway if the drag never starts.
	std::thread hand([&] {
		for (int i = 0; i < 250 && !g_drag.started; i++) Sleep(20);
		if (g_drag.started) {
			const int steps = 15;
			for (int i = 1; i <= steps; i++) {
				SetCursorPos(from.x + (to.x - from.x) * i / steps, from.y + (to.y - from.y) * i / steps);
				Sleep(20);
			}
			for (int i = 0; i < 10; i++) {
				SetCursorPos(to.x + (i & 1), to.y);
				Sleep(30);
			}
			SetCursorPos(to.x, to.y);
			Sleep(50);
		}
		SendButton(MOUSEEVENTF_LEFTUP);
	});
	SendButton(MOUSEEVENTF_LEFTDOWN);
	const ULONGLONG deadline = GetTickCount64() + 10000;
	while (!g_drag.finished && GetTickCount64() < deadline) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		Sleep(10);
	}
	hand.join();
	DestroyWindow(source);
	OleUninitialize();
	if (!g_drag.started) {
		std::fprintf(stderr, "the drag did not start (the source window got no button press)\n");
		return 1;
	}
	if (g_drag.hr != DRAGDROP_S_DROP || g_drag.effect == DROPEFFECT_NONE) {
		std::fprintf(stderr, "drop refused (hr 0x%08lx, effect %lu)\n", static_cast<unsigned long>(g_drag.hr),
					 g_drag.effect);
		return 1;
	}
	return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // the GUI's pixels, unscaled
	const char* usage = "usage: gui_input drop <hwnd> <x> <y> <file>... | click <hwnd> <x> <y>\n";
	if (argc < 5) {
		std::fprintf(stderr, "%s", usage);
		return 2;
	}
	const std::wstring cmd = argv[1];
	HWND hwnd = reinterpret_cast<HWND>(static_cast<INT_PTR>(_wcstoui64(argv[2], nullptr, 0)));
	if (!IsWindow(hwnd)) {
		std::fprintf(stderr, "not a window: %ls\n", argv[2]);
		return 2;
	}
	const POINT at = ScreenPoint(hwnd, _wtoi(argv[3]), _wtoi(argv[4]));

	if (cmd == L"click") {
		SetCursorPos(at.x, at.y);
		Sleep(100);
		SendButton(MOUSEEVENTF_LEFTDOWN);
		Sleep(100);
		SendButton(MOUSEEVENTF_LEFTUP);
		return 0;
	}
	if (cmd != L"drop" || argc < 6) {
		std::fprintf(stderr, "%s", usage);
		return 2;
	}
	std::vector<std::wstring> files;
	for (int i = 5; i < argc; i++) {
		wchar_t full[MAX_PATH * 4];
		GetFullPathNameW(argv[i], static_cast<DWORD>(std::size(full)), full, nullptr);
		files.push_back(full);
	}
	// The drag starts beside the window's top-left corner, outside it, as from an Explorer window.
	RECT r;
	GetWindowRect(hwnd, &r);
	const POINT from{r.left > 40 ? r.left - 20 : r.right + 20, r.top + 40};
	return DragFiles(files, from, at);
}
