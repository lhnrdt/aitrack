#include "ImGuiView.h"

#include "../presenter/i_presenter.h"
#include "../version.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace
{
	constexpr UINT TRACKING_HOTKEY_ID = 1;
	constexpr UINT RENDER_TIMER_ID = 2;
	constexpr float MAIN_WINDOW_WIDTH = 420.0f;
	constexpr float MAIN_WINDOW_HEIGHT_WITH_PREVIEW = 459.0f;
	constexpr float MAIN_WINDOW_HEIGHT_WITH_DIAGNOSTICS = 86.0f;
	constexpr float MAIN_WINDOW_HEIGHT_COMPACT = 198.0f;
	constexpr int MAIN_WINDOW_MARGIN = 8;

	std::wstring widen(const char* value)
	{
		int size = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
		std::wstring result(size - 1, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value, -1, &result[0], size);
		return result;
	}

	void setBuffer(std::array<char, 16>& buffer, int value)
	{
		std::snprintf(buffer.data(), buffer.size(), "%d", value);
	}

	void setBuffer(std::array<char, 32>& buffer, double value)
	{
		std::snprintf(buffer.data(), buffer.size(), "%.6g", value);
	}

	void setBuffer(std::array<char, 64>& buffer, const std::string& value)
	{
		std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
	}

	int parseInt(const char* value, int fallback)
	{
		try { return std::stoi(value); }
		catch (...) { return fallback; }
	}

	double parseDouble(const char* value, double fallback)
	{
		try { return std::stod(value); }
		catch (...) { return fallback; }
	}
}

ImGuiView::ImGuiView()
{
	WNDCLASSEXW wc = {
		sizeof(WNDCLASSEXW), CS_CLASSDC, ImGuiView::wndProc, 0L, 0L,
		GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
		L"AITrackImGui", nullptr
	};
	RegisterClassExW(&wc);
	WNDCLASSEXW config_wc = {
		sizeof(WNDCLASSEXW), CS_CLASSDC, ImGuiView::configWndProc, 0L, 0L,
		GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
		L"AITrackImGuiConfig", nullptr
	};
	RegisterClassExW(&config_wc);
	WNDCLASSEXW calibration_wc = {
		sizeof(WNDCLASSEXW), CS_CLASSDC, ImGuiView::calibrationWndProc, 0L, 0L,
		GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
		L"AITrackImGuiCalibration", nullptr
	};
	RegisterClassExW(&calibration_wc);

	RECT window_rect = { 0, 0, static_cast<LONG>(MAIN_WINDOW_WIDTH) + MAIN_WINDOW_MARGIN * 2,
		static_cast<LONG>(MAIN_WINDOW_HEIGHT_WITH_PREVIEW) + MAIN_WINDOW_MARGIN * 2 };
	AdjustWindowRect(&window_rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

	std::wstring title = widen("AITrack " AITRACK_VERSION);
	hwnd = CreateWindowW(wc.lpszClassName, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		100, 100, window_rect.right - window_rect.left, window_rect.bottom - window_rect.top, nullptr, nullptr, wc.hInstance, this);
	host_show_video_feed = true;

	if (!createDeviceD3D(hwnd))
		throw std::runtime_error("Could not create Direct3D device");

	ShowWindow(hwnd, SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	main_context = ImGui::CreateContext();
	ImGui::SetCurrentContext(main_context);
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	applyThemeForCurrentContext();

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(d3d_device.Get(), d3d_context.Get());
}

ImGuiView::~ImGuiView()
{
	stopOperationThreads();
	registerTrackingShortcut(false);
	destroyConfigWindow();
	destroyCalibrationWindow();
	ImGui::SetCurrentContext(main_context);
	releaseCalibrationTexture();
	releaseVideoTexture();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext(main_context);
	main_context = nullptr;
	cleanupDeviceD3D();
	if (hwnd)
		DestroyWindow(hwnd);
	UnregisterClassW(L"AITrackImGuiCalibration", GetModuleHandle(nullptr));
	UnregisterClassW(L"AITrackImGuiConfig", GetModuleHandle(nullptr));
	UnregisterClassW(L"AITrackImGui", GetModuleHandle(nullptr));
}

int ImGuiView::run()
{
	MSG msg;
	while (running)
	{
		ui_thread_id = GetCurrentThreadId();
		while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				running = false;
		}
		if (!running)
			break;

		renderFrame();
	}

	if (presenter)
		presenter->close_program();
	return 0;
}

void ImGuiView::renderFrame()
{
	processPendingUiRequests();
	joinCompletedOperationThreads();

	ImGui::SetCurrentContext(main_context);
	uploadPendingFrame();

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	renderMainWindow();
	updateMouseCursor();

	ImGui::Render();
	const float clear_color[4] = { 0.94f, 0.94f, 0.94f, 1.0f };
	d3d_context->OMSetRenderTargets(1, render_target_view.GetAddressOf(), nullptr);
	d3d_context->ClearRenderTargetView(render_target_view.Get(), clear_color);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	swap_chain->Present(1, 0);
	renderConfigWindow();
	renderCalibrationWindow();
}

void ImGuiView::connect_presenter(IPresenter* presenter)
{
	this->presenter = presenter;
}

void ImGuiView::show_tracking_data(ConfigData conf)
{
	if (!isUiThread())
	{
		std::lock_guard<std::mutex> lock(pending_ui_mutex);
		pending_tracking_data = conf;
		has_pending_tracking_data = true;
		return;
	}
	state.x = conf.x;
	state.y = conf.y;
	state.z = conf.z;
	state.yaw = conf.yaw;
	state.pitch = conf.pitch;
	state.roll = conf.roll;
}

void ImGuiView::set_tracking_mode(bool is_tracking)
{
	if (!isUiThread())
	{
		queueTrackingMode(is_tracking);
		return;
	}
	tracking = is_tracking;
	if (!tracking)
		releaseVideoTexture();
}

ConfigData ImGuiView::get_inputs()
{
	syncStateFromBuffers();
	return state;
}

void ImGuiView::update_view_state(ConfigData conf)
{
	if (!isUiThread())
	{
		queueViewState(conf);
		return;
	}
	state = conf;
	syncBuffersFromState();
	registerTrackingShortcut(state.tracking_shortcut_enabled);
	applyCurrentTheme();
	resizeHostWindowForMainContent(true);
}

void ImGuiView::set_enabled(bool enabled)
{
	if (!isUiThread())
	{
		queueEnabled(enabled);
		return;
	}
	this->enabled = enabled;
}

void ImGuiView::set_visible(bool visible)
{
	if (!isUiThread())
	{
		queueVisible(visible);
		return;
	}
	if (calibration_visible || calibration_hwnd)
	{
		calibration_visible = visible;
		if (!visible && calibration_hwnd)
		{
			ShowWindow(calibration_hwnd, SW_HIDE);
			releaseCalibrationTexture();
		}
		return;
	}

	running = visible;
}

void ImGuiView::show_message(const char* msg, MSG_SEVERITY severity)
{
	if (!isUiThread())
	{
		queueMessage(msg ? msg : "", severity);
		return;
	}
	MessageBoxA(hwnd, msg, severity == CRITICAL ? "Warning" : "Information",
		MB_OK | (severity == CRITICAL ? MB_ICONWARNING : MB_ICONINFORMATION));
}

void ImGuiView::set_shortcuts(bool enabled)
{
	if (!isUiThread())
	{
		std::lock_guard<std::mutex> lock(pending_ui_mutex);
		pending_shortcuts = enabled;
		has_pending_shortcuts = true;
		return;
	}
	registerTrackingShortcut(enabled);
}

IView* ImGuiView::get_calibration_window()
{
	return this;
}

void ImGuiView::show_frame_performance(const FramePerformanceData& data)
{
	diagnostic_capture_ms.store(data.capture_ms, std::memory_order_relaxed);
	diagnostic_preprocess_ms.store(data.preprocess_ms, std::memory_order_relaxed);
	diagnostic_inference_ms.store(data.inference_ms, std::memory_order_relaxed);
	diagnostic_output_ms.store(data.output_ms, std::memory_order_relaxed);
	diagnostic_wait_ms.store(data.wait_ms, std::memory_order_relaxed);
}

void ImGuiView::paint_video_frame(cv::Mat& img)
{
	std::lock_guard<std::mutex> lock(frame_mutex);
	const auto now = std::chrono::steady_clock::now();
	if (has_diagnostic_frame_time)
	{
		const float frame_time_ms = std::chrono::duration<float, std::milli>(now - diagnostic_last_frame_time).count();
		if (frame_time_ms > 0.0f)
		{
			diagnostic_frame_time_ms.store(frame_time_ms, std::memory_order_relaxed);
			diagnostic_fps.store(1000.0f / frame_time_ms, std::memory_order_relaxed);
		}
	}
	diagnostic_last_frame_time = now;
	has_diagnostic_frame_time = true;

	if (img.channels() == 3)
	{
		if (calibration_visible)
			cv::cvtColor(img, calibration_pending_frame, cv::COLOR_RGB2RGBA);
		else
			cv::cvtColor(img, pending_frame, cv::COLOR_RGB2RGBA);
	}
	else
	{
		if (calibration_visible)
			img.copyTo(calibration_pending_frame);
		else
			img.copyTo(pending_frame);
	}

	if (calibration_visible)
		calibration_frame_dirty = true;
	else
		frame_dirty = true;
}

void ImGuiView::notify(IView* self)
{
	applyPrefs();
}

bool ImGuiView::createDeviceD3D(HWND hwnd)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT create_device_flags = 0;
	D3D_FEATURE_LEVEL feature_level;
	const D3D_FEATURE_LEVEL feature_level_array[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_device_flags,
		feature_level_array, 2, D3D11_SDK_VERSION, &sd, &swap_chain, &d3d_device, &feature_level, &d3d_context);
	if (FAILED(result))
		return false;

	createRenderTarget();
	return true;
}

bool ImGuiView::createSwapChainForWindow(HWND target_hwnd, Microsoft::WRL::ComPtr<IDXGISwapChain>& target_swap_chain)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = target_hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
	Microsoft::WRL::ComPtr<IDXGIFactory> factory;
	if (FAILED(d3d_device.As(&dxgi_device)) ||
		FAILED(dxgi_device->GetAdapter(&adapter)) ||
		FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
		return false;

	return SUCCEEDED(factory->CreateSwapChain(d3d_device.Get(), &sd, &target_swap_chain));
}

void ImGuiView::cleanupDeviceD3D()
{
	cleanupRenderTarget();
	config_swap_chain.Reset();
	calibration_swap_chain.Reset();
	swap_chain.Reset();
	d3d_context.Reset();
	d3d_device.Reset();
}

void ImGuiView::createRenderTarget()
{
	createRenderTarget(swap_chain.Get(), render_target_view);
}

void ImGuiView::createRenderTarget(IDXGISwapChain* target_swap_chain, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& target_render_target_view)
{
	Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
	target_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
	d3d_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &target_render_target_view);
}

void ImGuiView::cleanupRenderTarget()
{
	render_target_view.Reset();
}

bool ImGuiView::createConfigWindow()
{
	if (config_hwnd)
		return true;

	RECT main_rect = {};
	GetWindowRect(hwnd, &main_rect);
	RECT window_rect = { 0, 0, 411, 492 };
	AdjustWindowRect(&window_rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

	config_hwnd = CreateWindowW(L"AITrackImGuiConfig", L"ConfigWindow",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		main_rect.right + 12, main_rect.top,
		window_rect.right - window_rect.left, window_rect.bottom - window_rect.top,
		nullptr, nullptr, GetModuleHandle(nullptr), this);
	if (!config_hwnd)
		return false;

	if (!createSwapChainForWindow(config_hwnd, config_swap_chain))
	{
		DestroyWindow(config_hwnd);
		config_hwnd = nullptr;
		return false;
	}
	createRenderTarget(config_swap_chain.Get(), config_render_target_view);

	ImGuiContext* previous_context = ImGui::GetCurrentContext();
	config_context = ImGui::CreateContext();
	ImGui::SetCurrentContext(config_context);
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	applyThemeForCurrentContext();
	ImGui_ImplWin32_Init(config_hwnd);
	ImGui_ImplDX11_Init(d3d_device.Get(), d3d_context.Get());
	ImGui::SetCurrentContext(previous_context);
	return true;
}

void ImGuiView::destroyConfigWindow()
{
	if (config_context)
	{
		ImGuiContext* previous_context = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(config_context);
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(config_context);
		config_context = nullptr;
		ImGui::SetCurrentContext(previous_context);
	}

	config_render_target_view.Reset();
	config_swap_chain.Reset();
	if (config_hwnd)
	{
		DestroyWindow(config_hwnd);
		config_hwnd = nullptr;
	}
}

bool ImGuiView::createCalibrationWindow()
{
	if (calibration_hwnd)
		return true;

	RECT main_rect = {};
	GetWindowRect(hwnd, &main_rect);
	RECT window_rect = { 0, 0, 488, 406 };
	AdjustWindowRect(&window_rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

	calibration_hwnd = CreateWindowW(L"AITrackImGuiCalibration", L"Head Calibration",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		main_rect.right + 12, main_rect.top + 36,
		window_rect.right - window_rect.left, window_rect.bottom - window_rect.top,
		nullptr, nullptr, GetModuleHandle(nullptr), this);
	if (!calibration_hwnd)
		return false;

	if (!createSwapChainForWindow(calibration_hwnd, calibration_swap_chain))
	{
		DestroyWindow(calibration_hwnd);
		calibration_hwnd = nullptr;
		return false;
	}
	createRenderTarget(calibration_swap_chain.Get(), calibration_render_target_view);

	ImGuiContext* previous_context = ImGui::GetCurrentContext();
	calibration_context = ImGui::CreateContext();
	ImGui::SetCurrentContext(calibration_context);
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	applyThemeForCurrentContext();
	ImGui_ImplWin32_Init(calibration_hwnd);
	ImGui_ImplDX11_Init(d3d_device.Get(), d3d_context.Get());
	ImGui::SetCurrentContext(previous_context);
	return true;
}

void ImGuiView::destroyCalibrationWindow()
{
	if (calibration_context)
	{
		ImGuiContext* previous_context = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(calibration_context);
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(calibration_context);
		calibration_context = nullptr;
		ImGui::SetCurrentContext(previous_context);
	}

	calibration_render_target_view.Reset();
	calibration_swap_chain.Reset();
	if (calibration_hwnd)
	{
		DestroyWindow(calibration_hwnd);
		calibration_hwnd = nullptr;
	}
}

void ImGuiView::uploadPendingFrame()
{
	cv::Mat frame;
	cv::Mat calibration_frame;
	{
		std::lock_guard<std::mutex> lock(frame_mutex);
		if (frame_dirty && !pending_frame.empty())
		{
			pending_frame.copyTo(frame);
			frame_dirty = false;
		}
		if (calibration_frame_dirty && !calibration_pending_frame.empty())
		{
			calibration_pending_frame.copyTo(calibration_frame);
			calibration_frame_dirty = false;
		}
	}

	if (!frame.empty())
		uploadFrame(frame, video_texture, video_texture_view, texture_width, texture_height);
	if (!calibration_frame.empty())
		uploadFrame(calibration_frame, calibration_texture, calibration_texture_view, calibration_texture_width, calibration_texture_height);
}

void ImGuiView::uploadFrame(cv::Mat& frame, Microsoft::WRL::ComPtr<ID3D11Texture2D>& target_texture,
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& target_texture_view,
	int& target_texture_width, int& target_texture_height)
{
	if (frame.cols != target_texture_width || frame.rows != target_texture_height || !target_texture)
	{
		target_texture_view.Reset();
		target_texture.Reset();
		target_texture_width = frame.cols;
		target_texture_height = frame.rows;

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = target_texture_width;
		desc.Height = target_texture_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		d3d_device->CreateTexture2D(&desc, nullptr, &target_texture);
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = desc.Format;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;
		d3d_device->CreateShaderResourceView(target_texture.Get(), &srv_desc, &target_texture_view);
	}

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(d3d_context->Map(target_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		for (int y = 0; y < frame.rows; ++y)
			memcpy(static_cast<unsigned char*>(mapped.pData) + mapped.RowPitch * y, frame.ptr(y), frame.cols * 4);
		d3d_context->Unmap(target_texture.Get(), 0);
	}
}

void ImGuiView::releaseVideoTexture()
{
	video_texture_view.Reset();
	video_texture.Reset();
	texture_width = 0;
	texture_height = 0;
	std::lock_guard<std::mutex> lock(frame_mutex);
	pending_frame.release();
	frame_dirty = false;
}

void ImGuiView::releaseCalibrationTexture()
{
	calibration_texture_view.Reset();
	calibration_texture.Reset();
	calibration_texture_width = 0;
	calibration_texture_height = 0;
}

void ImGuiView::resizeHostWindowForMainContent(bool force)
{
	if (!hwnd)
		return;

	const bool show_diagnostics = state.show_video_feed && state.show_diagnostics;
	if (!force && host_show_video_feed == state.show_video_feed && host_show_diagnostics == show_diagnostics)
		return;

	host_show_video_feed = state.show_video_feed;
	host_show_diagnostics = show_diagnostics;
	const int client_width = static_cast<int>(MAIN_WINDOW_WIDTH) + MAIN_WINDOW_MARGIN * 2;
	const int client_height = static_cast<int>(state.show_video_feed ? MAIN_WINDOW_HEIGHT_WITH_PREVIEW +
		(show_diagnostics ? MAIN_WINDOW_HEIGHT_WITH_DIAGNOSTICS : 0.0f) : MAIN_WINDOW_HEIGHT_COMPACT) +
		MAIN_WINDOW_MARGIN * 2;

	RECT window_rect = { 0, 0, client_width, client_height };
	AdjustWindowRect(&window_rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
	SetWindowPos(hwnd, nullptr, 0, 0, window_rect.right - window_rect.left, window_rect.bottom - window_rect.top,
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ImGuiView::renderMainWindow()
{
	const bool tracking_busy = tracking_operation.load();
	const bool waiting_for_camera_frame = tracking && state.show_video_feed && !video_texture_view;
	if (!tracking)
		releaseVideoTexture();
	const float main_window_height = state.show_video_feed ? MAIN_WINDOW_HEIGHT_WITH_PREVIEW +
		(state.show_diagnostics ? MAIN_WINDOW_HEIGHT_WITH_DIAGNOSTICS : 0.0f) : MAIN_WINDOW_HEIGHT_COMPACT;
	ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(MAIN_WINDOW_WIDTH, main_window_height), ImGuiCond_Always);
	ImGui::Begin("AITrack " AITRACK_VERSION, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

	if (state.show_video_feed)
	{
		renderVideoPanel(tracking ? video_texture_view.Get() : nullptr,
		tracking ? "Waiting for camera frame..." : "Tracking stopped");
		if (state.show_diagnostics)
			renderPerformanceChart();
	}

	ImGui::BeginDisabled(!enabled || tracking_busy);
	const char* tracking_label = tracking_busy ? "Working..." :
		(waiting_for_camera_frame ? "Waiting for camera frame..." : (tracking ? "Stop tracking" : "Start tracking"));
	if (ImGui::Button(tracking_label, ImVec2(-1, 40)) && presenter)
		startTrackingOperation();
	if (waiting_for_camera_frame)
		renderSpinnerOnLastItem();
	ImGui::EndDisabled();
	if (tracking_busy)
		renderSpinner("Changing tracking state");

	if (ImGui::Checkbox("Enable preview", &state.show_video_feed))
	{
		resizeHostWindowForMainContent();
		applyPrefs();
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Enable diagnostics", &state.show_diagnostics))
	{
		resizeHostWindowForMainContent(true);
		applyPrefs();
	}

	ImGui::BeginDisabled(tracking);
	if (ImGui::Button("Configuration", ImVec2(-1, 30)))
		config_visible = true;
	ImGui::EndDisabled();

	ImGui::End();
}

void ImGuiView::renderConfigWindow()
{
	if (!config_visible)
	{
		if (config_hwnd)
			ShowWindow(config_hwnd, SW_HIDE);
		return;
	}

	if (!createConfigWindow())
		return;

	ShowWindow(config_hwnd, SW_SHOW);
	UpdateWindow(config_hwnd);

	ImGuiContext* previous_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(config_context);

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
	ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
	ImGui::Begin("ConfigWindowContent", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	renderConfigContent();
	ImGui::End();

	ImGui::Render();
	const float clear_color[4] = { 0.94f, 0.94f, 0.94f, 1.0f };
	d3d_context->OMSetRenderTargets(1, config_render_target_view.GetAddressOf(), nullptr);
	d3d_context->ClearRenderTargetView(config_render_target_view.Get(), clear_color);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	config_swap_chain->Present(1, 0);

	updateMouseCursor();
	ImGui::SetCurrentContext(previous_context);
}

void ImGuiView::renderConfigContent()
{
	const bool apply_busy = apply_operation.load();
	const bool calibration_busy = calibration_operation.load();
	ImGui::BeginDisabled(!enabled || tracking || apply_busy || calibration_busy);
	constexpr ImGuiWindowFlags fixed_panel_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

	auto inputIntRow = [](const char* label, int* value) {
		ImGui::PushID(label);
		ImGui::TextUnformatted(label);
		ImGui::SameLine(76.0f);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::InputInt("##value", value);
		ImGui::PopID();
	};
	auto inputTextRow = [](const char* label, std::array<char, 32>& buffer, ImGuiInputTextFlags flags = 0) {
		ImGui::PushID(label);
		ImGui::TextUnformatted(label);
		ImGui::SameLine(88.0f);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::InputText("##value", buffer.data(), buffer.size(), flags);
		ImGui::PopID();
	};
	auto checkboxWrapped = [](const char* label, bool* value) {
		ImGui::PushID(label);
		ImGui::Checkbox("##value", value);
		ImGui::SameLine();
		ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
		ImGui::TextUnformatted(label);
		ImGui::PopTextWrapPos();
		ImGui::PopID();
	};

	ImGui::BeginChild("Camera", ImVec2(191, 331), true, fixed_panel_flags);
	ImGui::TextUnformatted("Camera");
	if (ImGui::BeginCombo("##camera", (std::string("Camera ") + std::to_string(state.selected_camera)).c_str()))
	{
		for (int i = 0; i < state.num_cameras_detected; ++i)
		{
			std::string label = "Camera " + std::to_string(i);
			if (ImGui::Selectable(label.c_str(), state.selected_camera == i))
				state.selected_camera = i;
		}
		ImGui::EndCombo();
	}
	inputIntRow("Width", &state.video_width);
	inputIntRow("Height", &state.video_height);
	inputIntRow("FPS", &state.video_fps);
	ImGui::Separator();
	ImGui::Checkbox("Face Auto-Exposure", &state.face_auto_exposure);
	ImGui::BeginDisabled(state.face_auto_exposure);
	bool custom_brightness = state.cam_gain > 0 && state.cam_exposure > 0;
	if (ImGui::Checkbox("Custom brightness", &custom_brightness))
	{
		state.cam_gain = custom_brightness ? 1 : -1;
		state.cam_exposure = custom_brightness ? 144 : -1;
	}
	if (custom_brightness)
	{
		ImGui::SliderInt("Gain", &state.cam_gain, 1, 64);
		ImGui::SliderInt("Exposure", &state.cam_exposure, 0, 254);
	}
	ImGui::EndDisabled();
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginGroup();
	ImGui::BeginChild("Remote", ImVec2(191, 92), true, fixed_panel_flags);
	ImGui::TextWrapped("Use remote OpenTrack client");
	ImGui::PushID("IP");
	ImGui::TextUnformatted("IP");
	ImGui::SameLine(54.0f);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	ImGui::InputText("##value", ip_buffer.data(), ip_buffer.size());
	ImGui::PopID();
	ImGui::PushID("Port");
	ImGui::TextUnformatted("Port");
	ImGui::SameLine(54.0f);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	ImGui::InputText("##value", port_buffer.data(), port_buffer.size(), ImGuiInputTextFlags_CharsDecimal);
	ImGui::PopID();
	ImGui::EndChild();

	ImGui::BeginChild("Tracker parameters", ImVec2(191, 196), true, fixed_panel_flags);
	ImGui::TextUnformatted("Tracker parameters");
	inputTextRow("Distance", distance_buffer, ImGuiInputTextFlags_CharsDecimal);
	inputTextRow("Camera FOV", fov_buffer, ImGuiInputTextFlags_CharsDecimal);
	const char* current_model = state.model_names.empty() ? "" : state.model_names[state.selected_model].c_str();
	ImGui::TextUnformatted("Model type");
	ImGui::SameLine(88.0f);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	if (ImGui::BeginCombo("##modelType", current_model))
	{
		for (int i = 0; i < static_cast<int>(state.model_names.size()); ++i)
		{
			if (ImGui::Selectable(state.model_names[i].c_str(), state.selected_model == i))
				state.selected_model = i;
		}
		ImGui::EndCombo();
	}
	checkboxWrapped("Landmark stabilization", &state.use_landmark_stab);
	if (ImGui::Button(calibration_busy ? "Calibrating..." : "Calibrate Face") && !calibration_busy)
		calibration_visible = true;
	if (calibration_busy)
		renderSpinner("Preparing calibration");
	ImGui::EndChild();

	ImGui::BeginChild("General", ImVec2(191, 119), true, fixed_panel_flags);
	ImGui::TextUnformatted("General");
	checkboxWrapped("Autocheck updates", &state.autocheck_updates);
	checkboxWrapped("Start/Stop Tracking shortcut", &state.tracking_shortcut_enabled);
	if (ImGui::Checkbox("Dark mode", &state.dark_mode))
		applyCurrentTheme();
	ImGui::EndChild();
	ImGui::EndGroup();

	if (ImGui::Button(apply_busy ? "Applying..." : "Apply", ImVec2(-1, 31)) && !apply_busy)
		applyPrefs();
	if (apply_busy)
		renderSpinner("Applying settings");
	ImGui::EndDisabled();
}

void ImGuiView::renderCalibrationWindow()
{
	if (!calibration_visible)
	{
		if (calibration_hwnd)
			ShowWindow(calibration_hwnd, SW_HIDE);
		return;
	}

	if (!createCalibrationWindow())
		return;

	ShowWindow(calibration_hwnd, SW_SHOW);
	UpdateWindow(calibration_hwnd);

	ImGuiContext* previous_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(calibration_context);

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
	ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
	ImGui::Begin("HeadCalibrationContent", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

	renderVideoPanel(calibration_texture_view.Get(), "Look directly at the camera, click \"Calibrate\" and wait a few seconds");
	const bool calibration_busy = calibration_operation.load();
	ImGui::BeginDisabled(calibration_busy || !enabled);
	if (ImGui::Button(calibration_busy ? "Calibrating..." : "Calibrate", ImVec2(-1, 40)) && presenter)
	{
		startCalibrationOperation();
	}
	ImGui::EndDisabled();
	if (calibration_busy)
		renderSpinner("Calibrating face");
	ImGui::End();

	ImGui::Render();
	const float clear_color[4] = { 0.94f, 0.94f, 0.94f, 1.0f };
	d3d_context->OMSetRenderTargets(1, calibration_render_target_view.GetAddressOf(), nullptr);
	d3d_context->ClearRenderTargetView(calibration_render_target_view.Get(), clear_color);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	calibration_swap_chain->Present(1, 0);

	updateMouseCursor();
	ImGui::SetCurrentContext(previous_context);
}

void ImGuiView::updateMouseCursor()
{
	SetCursor(LoadCursor(nullptr, ImGui::IsAnyItemHovered() ? IDC_HAND : IDC_ARROW));
}

void ImGuiView::renderVideoPanel(ID3D11ShaderResourceView* texture, const char* empty_text)
{
	ImVec2 size(400, 280);
	ImGui::BeginChild("cameraView", size, false, ImGuiWindowFlags_NoScrollbar);
	const ImVec2 panel_position = ImGui::GetCursorScreenPos();
	if (texture)
		ImGui::Image(reinterpret_cast<ImTextureID>(texture), size);
	else
	{
		ImGui::SetCursorPosY((size.y - ImGui::GetTextLineHeightWithSpacing()) * 0.5f);
		ImGui::SetCursorPosX(12.0f);
		ImGui::TextWrapped("%s", empty_text);
	}
	if (texture && state.show_diagnostics)
	{
		const float fps = diagnostic_fps.load(std::memory_order_relaxed);
		const float frame_time_ms = diagnostic_frame_time_ms.load(std::memory_order_relaxed);
		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		draw_list->AddRectFilled(ImVec2(panel_position.x + 6.0f, panel_position.y + 6.0f),
			ImVec2(panel_position.x + 132.0f, panel_position.y + 43.0f), IM_COL32(0, 0, 0, 170), 3.0f);
		char diagnostics[64];
		std::snprintf(diagnostics, sizeof(diagnostics), "FPS %.1f\nFrame %.1f ms", fps, frame_time_ms);
		draw_list->AddText(ImVec2(panel_position.x + 12.0f, panel_position.y + 10.0f), IM_COL32(255, 255, 255, 255), diagnostics);
	}
	ImGui::EndChild();
}

void ImGuiView::renderPerformanceChart()
{
	const float values[] = {
		diagnostic_capture_ms.load(std::memory_order_relaxed),
		diagnostic_preprocess_ms.load(std::memory_order_relaxed),
		diagnostic_inference_ms.load(std::memory_order_relaxed),
		diagnostic_output_ms.load(std::memory_order_relaxed),
		diagnostic_wait_ms.load(std::memory_order_relaxed)
	};
	const char* labels[] = { "Capture", "Preprocess", "Inference", "Output", "Wait" };
	const ImU32 colors[] = {
		IM_COL32(70, 130, 180, 255),
		IM_COL32(218, 165, 32, 255),
		IM_COL32(198, 92, 92, 255),
		IM_COL32(93, 153, 85, 255),
		IM_COL32(130, 130, 130, 255)
	};
	float total = 0.0f;
	for (float value : values)
		total += value;

	ImGui::TextUnformatted("Frame phases");
	ImGui::InvisibleButton("framePerformanceChart", ImVec2(-1.0f, 64.0f));
	const ImVec2 chart_min = ImGui::GetItemRectMin();
	const ImVec2 chart_max = ImGui::GetItemRectMax();
	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	const float bar_top = chart_min.y + 2.0f;
	const float bar_bottom = bar_top + 22.0f;
	draw_list->AddRectFilled(ImVec2(chart_min.x, bar_top), ImVec2(chart_max.x, bar_bottom), IM_COL32(45, 45, 45, 255), 2.0f);

	if (total > 0.0f)
	{
		float segment_start = chart_min.x;
		for (int i = 0; i < 5; ++i)
		{
			const float segment_width = (chart_max.x - chart_min.x) * values[i] / total;
			if (segment_width > 0.0f)
			{
				draw_list->AddRectFilled(ImVec2(segment_start, bar_top),
					ImVec2(segment_start + segment_width, bar_bottom), colors[i]);
				segment_start += segment_width;
			}
		}
	}

	for (int i = 0; i < 5; ++i)
	{
		const float legend_x = chart_min.x + (i % 3) * 136.0f;
		const float legend_y = chart_min.y + 31.0f + (i / 3) * 16.0f;
		char legend[48];
		std::snprintf(legend, sizeof(legend), "%s %.1f ms", labels[i], values[i]);
		draw_list->AddRectFilled(ImVec2(legend_x, legend_y + 2.0f), ImVec2(legend_x + 8.0f, legend_y + 10.0f), colors[i]);
		draw_list->AddText(ImVec2(legend_x + 12.0f, legend_y), ImGui::GetColorU32(ImGuiCol_Text), legend);
	}
}

void ImGuiView::renderSpinner(const char* label)
{
	const float radius = 7.0f;
	const ImVec2 center(ImGui::GetCursorScreenPos().x + radius + 2.0f,
		ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f);
	const float start = static_cast<float>(ImGui::GetTime()) * 5.0f;
	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	for (int i = 0; i < 8; ++i)
	{
		const float angle = start + static_cast<float>(i) * 3.14159265f / 4.0f;
		const float alpha = static_cast<float>(i + 1) / 8.0f;
		const ImVec2 point(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
		draw_list->AddCircleFilled(point, 2.0f, ImGui::GetColorU32(ImGuiCol_Text, alpha));
	}
	ImGui::Dummy(ImVec2(radius * 2.0f + 8.0f, ImGui::GetTextLineHeight()));
	ImGui::SameLine();
	ImGui::TextUnformatted(label);
}

void ImGuiView::renderSpinnerOnLastItem()
{
	const ImVec2 min = ImGui::GetItemRectMin();
	const ImVec2 max = ImGui::GetItemRectMax();
	const ImVec2 center(min.x + 18.0f, (min.y + max.y) * 0.5f);
	const float start = static_cast<float>(ImGui::GetTime()) * 5.0f;
	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	for (int i = 0; i < 8; ++i)
	{
		const float angle = start + static_cast<float>(i) * 3.14159265f / 4.0f;
		const float alpha = static_cast<float>(i + 1) / 8.0f;
		const ImVec2 point(center.x + std::cos(angle) * 7.0f, center.y + std::sin(angle) * 7.0f);
		draw_list->AddCircleFilled(point, 2.0f, ImGui::GetColorU32(ImGuiCol_ButtonActive, alpha));
	}
}

void ImGuiView::syncBuffersFromState()
{
	setBuffer(ip_buffer, state.ip);
	setBuffer(port_buffer, state.port);
	setBuffer(distance_buffer, state.prior_distance);
	setBuffer(fov_buffer, state.camera_fov);
}

void ImGuiView::syncStateFromBuffers()
{
	state.ip = ip_buffer.data();
	state.port = parseInt(port_buffer.data(), 4242);
	state.prior_distance = parseDouble(distance_buffer.data(), state.prior_distance);
	state.camera_fov = parseDouble(fov_buffer.data(), state.camera_fov);
	if (state.ip.empty())
		state.ip = "127.0.0.1";
	if (state.port == 0)
		state.port = 4242;
}

void ImGuiView::startTrackingOperation()
{
	if (!presenter || tracking_operation.exchange(true))
		return;
	tracking_operation_thread = std::thread([this]() {
		try
		{
			presenter->toggle_tracking();
		}
		catch (const std::exception& ex)
		{
			queueMessage(ex.what(), CRITICAL);
		}
		catch (...)
		{
			queueMessage("The tracking operation failed.", CRITICAL);
		}
		tracking_operation = false;
	});
}

void ImGuiView::startApplyOperation()
{
	if (!presenter || apply_operation.exchange(true))
		return;
	const ConfigData requested_state = state;
	apply_operation_thread = std::thread([this, requested_state]() {
		try
		{
			presenter->save_prefs(requested_state);
		}
		catch (const std::exception& ex)
		{
			queueMessage(ex.what(), CRITICAL);
		}
		catch (...)
		{
			queueMessage("Applying settings failed.", CRITICAL);
		}
		apply_operation = false;
	});
}

void ImGuiView::startCalibrationOperation()
{
	if (!presenter || calibration_operation.exchange(true))
		return;
	const ConfigData requested_state = state;
	calibration_operation_thread = std::thread([this, requested_state]() {
		try
		{
			presenter->save_prefs(requested_state);
			presenter->calibrate_face(*this);
		}
		catch (const std::exception& ex)
		{
			queueMessage(ex.what(), CRITICAL);
		}
		catch (...)
		{
			queueMessage("Face calibration failed.", CRITICAL);
		}
		calibration_operation = false;
	});
}

void ImGuiView::joinCompletedOperationThreads()
{
	if (!tracking_operation.load() && tracking_operation_thread.joinable())
		tracking_operation_thread.join();
	if (!apply_operation.load() && apply_operation_thread.joinable())
		apply_operation_thread.join();
	if (!calibration_operation.load() && calibration_operation_thread.joinable())
		calibration_operation_thread.join();
}

void ImGuiView::stopOperationThreads()
{
	if (tracking_operation_thread.joinable())
		tracking_operation_thread.join();
	if (apply_operation_thread.joinable())
		apply_operation_thread.join();
	if (calibration_operation_thread.joinable())
		calibration_operation_thread.join();
}

bool ImGuiView::isUiThread() const
{
	return ui_thread_id == 0 || ui_thread_id == GetCurrentThreadId();
}

void ImGuiView::queueTrackingMode(bool is_tracking)
{
	std::lock_guard<std::mutex> lock(pending_ui_mutex);
	pending_tracking_mode = is_tracking;
	has_pending_tracking_mode = true;
}

void ImGuiView::queueViewState(ConfigData conf)
{
	std::lock_guard<std::mutex> lock(pending_ui_mutex);
	pending_view_state = conf;
	has_pending_view_state = true;
}

void ImGuiView::queueEnabled(bool value)
{
	std::lock_guard<std::mutex> lock(pending_ui_mutex);
	pending_enabled = value;
	has_pending_enabled = true;
}

void ImGuiView::queueVisible(bool value)
{
	std::lock_guard<std::mutex> lock(pending_ui_mutex);
	pending_visible = value;
	has_pending_visible = true;
}

void ImGuiView::queueMessage(std::string msg, MSG_SEVERITY severity)
{
	std::lock_guard<std::mutex> lock(pending_ui_mutex);
	pending_message = std::move(msg);
	pending_message_severity = severity;
	has_pending_message = true;
}

void ImGuiView::processPendingUiRequests()
{
	if (!isUiThread())
		return;

	std::lock_guard<std::mutex> lock(pending_ui_mutex);
	if (has_pending_tracking_data)
	{
		state.x = pending_tracking_data.x;
		state.y = pending_tracking_data.y;
		state.z = pending_tracking_data.z;
		state.yaw = pending_tracking_data.yaw;
		state.pitch = pending_tracking_data.pitch;
		state.roll = pending_tracking_data.roll;
		has_pending_tracking_data = false;
	}
	if (has_pending_tracking_mode)
	{
		set_tracking_mode(pending_tracking_mode);
		has_pending_tracking_mode = false;
	}
	if (has_pending_view_state)
	{
		const ConfigData value = pending_view_state;
		has_pending_view_state = false;
		state = value;
		syncBuffersFromState();
		registerTrackingShortcut(state.tracking_shortcut_enabled);
		applyCurrentTheme();
		resizeHostWindowForMainContent(true);
	}
	if (has_pending_enabled)
	{
		enabled = pending_enabled;
		has_pending_enabled = false;
	}
	if (has_pending_visible)
	{
		const bool value = pending_visible;
		has_pending_visible = false;
		set_visible(value);
	}
	if (has_pending_shortcuts)
	{
		registerTrackingShortcut(pending_shortcuts);
		has_pending_shortcuts = false;
	}
	if (has_pending_message)
	{
		const std::string msg = pending_message;
		const MSG_SEVERITY severity = pending_message_severity;
		has_pending_message = false;
		MessageBoxA(hwnd, msg.c_str(), severity == CRITICAL ? "Warning" : "Information",
			MB_OK | (severity == CRITICAL ? MB_ICONWARNING : MB_ICONINFORMATION));
	}
}

void ImGuiView::applyPrefs()
{
	if (!presenter)
		return;
	syncStateFromBuffers();
	startApplyOperation();
}

void ImGuiView::applyCurrentTheme()
{
	ImGuiContext* previous_context = ImGui::GetCurrentContext();

	if (main_context)
	{
		ImGui::SetCurrentContext(main_context);
		applyThemeForCurrentContext();
	}
	if (config_context)
	{
		ImGui::SetCurrentContext(config_context);
		applyThemeForCurrentContext();
	}
	if (calibration_context)
	{
		ImGui::SetCurrentContext(calibration_context);
		applyThemeForCurrentContext();
	}

	ImGui::SetCurrentContext(previous_context);
}

void ImGuiView::applyThemeForCurrentContext()
{
	if (state.dark_mode)
		ImGui::StyleColorsDark();
	else
		ImGui::StyleColorsLight();

	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 0.0f;
	style.FrameRounding = 2.0f;
	style.GrabRounding = 2.0f;
}

void ImGuiView::registerTrackingShortcut(bool enabled)
{
	if (enabled == shortcut_enabled)
		return;

	if (enabled)
		RegisterHotKey(hwnd, TRACKING_HOTKEY_ID, MOD_CONTROL, 'T');
	else
		UnregisterHotKey(hwnd, TRACKING_HOTKEY_ID);
	shortcut_enabled = enabled;
}

LRESULT WINAPI ImGuiView::wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	ImGuiView* view = reinterpret_cast<ImGuiView*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (msg == WM_NCCREATE)
	{
		CREATESTRUCT* create = reinterpret_cast<CREATESTRUCT*>(lparam);
		view = reinterpret_cast<ImGuiView*>(create->lpCreateParams);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}

	if (view && view->main_context)
	{
		ImGuiContext* previous_context = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(view->main_context);
		const LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
		ImGui::SetCurrentContext(previous_context);
		if (handled)
			return true;
	}

	switch (msg)
	{
	case WM_SETCURSOR:
		if (view && LOWORD(lparam) == HTCLIENT)
		{
			ImGuiContext* previous_context = ImGui::GetCurrentContext();
			ImGui::SetCurrentContext(view->main_context);
			view->updateMouseCursor();
			ImGui::SetCurrentContext(previous_context);
			return TRUE;
		}
		break;
	case WM_SIZE:
		if (view && view->d3d_device && wparam != SIZE_MINIMIZED)
		{
			view->cleanupRenderTarget();
			view->swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
			view->createRenderTarget();
		}
		return 0;
	case WM_ENTERSIZEMOVE:
		if (view)
			SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
		return 0;
	case WM_EXITSIZEMOVE:
		KillTimer(hwnd, RENDER_TIMER_ID);
		return 0;
	case WM_TIMER:
		if (view && wparam == RENDER_TIMER_ID)
		{
			view->renderFrame();
			return 0;
		}
		break;
	case WM_HOTKEY:
		if (view && wparam == TRACKING_HOTKEY_ID && view->presenter)
			view->presenter->toggle_tracking();
		return 0;
	case WM_DESTROY:
		if (view)
			view->running = false;
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hwnd, msg, wparam, lparam);
}

LRESULT WINAPI ImGuiView::configWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	ImGuiView* view = reinterpret_cast<ImGuiView*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (msg == WM_NCCREATE)
	{
		CREATESTRUCT* create = reinterpret_cast<CREATESTRUCT*>(lparam);
		view = reinterpret_cast<ImGuiView*>(create->lpCreateParams);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}

	if (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wparam & 0xfff0) == SC_CLOSE))
	{
		if (view)
		{
			view->config_visible = false;
			ShowWindow(hwnd, SW_HIDE);
		}
		return 0;
	}

	if (view && view->config_context)
	{
		ImGuiContext* previous_context = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(view->config_context);
		const LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
		ImGui::SetCurrentContext(previous_context);
		if (handled)
			return true;
	}

	switch (msg)
	{
	case WM_SETCURSOR:
		if (view && LOWORD(lparam) == HTCLIENT)
		{
			ImGuiContext* previous_context = ImGui::GetCurrentContext();
			ImGui::SetCurrentContext(view->config_context);
			view->updateMouseCursor();
			ImGui::SetCurrentContext(previous_context);
			return TRUE;
		}
		break;
	case WM_SIZE:
		if (view && view->d3d_device && view->config_swap_chain && wparam != SIZE_MINIMIZED)
		{
			view->config_render_target_view.Reset();
			view->config_swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
			view->createRenderTarget(view->config_swap_chain.Get(), view->config_render_target_view);
		}
		return 0;
	case WM_DESTROY:
		if (view)
			view->config_hwnd = nullptr;
		return 0;
	}
	return DefWindowProc(hwnd, msg, wparam, lparam);
}

LRESULT WINAPI ImGuiView::calibrationWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	ImGuiView* view = reinterpret_cast<ImGuiView*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (msg == WM_NCCREATE)
	{
		CREATESTRUCT* create = reinterpret_cast<CREATESTRUCT*>(lparam);
		view = reinterpret_cast<ImGuiView*>(create->lpCreateParams);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}

	if (view && view->calibration_context)
	{
		ImGuiContext* previous_context = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(view->calibration_context);
		const LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
		ImGui::SetCurrentContext(previous_context);
		if (handled)
			return true;
	}

	switch (msg)
	{
	case WM_SETCURSOR:
		if (view && LOWORD(lparam) == HTCLIENT)
		{
			ImGuiContext* previous_context = ImGui::GetCurrentContext();
			ImGui::SetCurrentContext(view->calibration_context);
			view->updateMouseCursor();
			ImGui::SetCurrentContext(previous_context);
			return TRUE;
		}
		break;
	case WM_SIZE:
		if (view && view->d3d_device && view->calibration_swap_chain && wparam != SIZE_MINIMIZED)
		{
			view->calibration_render_target_view.Reset();
			view->calibration_swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
			view->createRenderTarget(view->calibration_swap_chain.Get(), view->calibration_render_target_view);
		}
		return 0;
	case WM_CLOSE:
		if (view)
		{
			view->calibration_visible = false;
			ShowWindow(hwnd, SW_HIDE);
			view->releaseCalibrationTexture();
		}
		return 0;
	case WM_DESTROY:
		if (view)
			view->calibration_hwnd = nullptr;
		return 0;
	}
	return DefWindowProc(hwnd, msg, wparam, lparam);
}
