#include "ImGuiView.h"

#include "../presenter/i_presenter.h"
#include "../version.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <opencv2/imgproc.hpp>

#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace
{
	constexpr UINT TRACKING_HOTKEY_ID = 1;

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

	std::wstring title = widen("AITrack " AITRACK_VERSION);
	hwnd = CreateWindowW(wc.lpszClassName, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		100, 100, 436, 498, nullptr, nullptr, wc.hInstance, this);

	if (!createDeviceD3D(hwnd))
		throw std::runtime_error("Could not create Direct3D device");

	ShowWindow(hwnd, SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGui::StyleColorsLight();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 0.0f;
	style.FrameRounding = 2.0f;
	style.GrabRounding = 2.0f;

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(d3d_device.Get(), d3d_context.Get());
}

ImGuiView::~ImGuiView()
{
	registerTrackingShortcut(false);
	releaseVideoTexture();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	cleanupDeviceD3D();
	if (hwnd)
		DestroyWindow(hwnd);
	UnregisterClassW(L"AITrackImGui", GetModuleHandle(nullptr));
}

int ImGuiView::run()
{
	MSG msg;
	while (running)
	{
		while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				running = false;
		}
		if (!running)
			break;

		uploadPendingFrame();

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		renderMainWindow();
		renderConfigWindow();
		renderCalibrationWindow();

		if (message_open)
			ImGui::OpenPopup(message_severity == CRITICAL ? "Warning" : "Information");
		if (ImGui::BeginPopupModal(message_severity == CRITICAL ? "Warning" : "Information", &message_open, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("%s", message_text.c_str());
			if (ImGui::Button("OK", ImVec2(120, 0)))
				message_open = false;
			ImGui::EndPopup();
		}

		ImGui::Render();
		const float clear_color[4] = { 0.94f, 0.94f, 0.94f, 1.0f };
		d3d_context->OMSetRenderTargets(1, render_target_view.GetAddressOf(), nullptr);
		d3d_context->ClearRenderTargetView(render_target_view.Get(), clear_color);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		swap_chain->Present(1, 0);
	}

	if (presenter)
		presenter->close_program();
	return 0;
}

void ImGuiView::connect_presenter(IPresenter* presenter)
{
	this->presenter = presenter;
}

void ImGuiView::show_tracking_data(ConfigData conf)
{
	state.x = conf.x;
	state.y = conf.y;
	state.z = conf.z;
	state.yaw = conf.yaw;
	state.pitch = conf.pitch;
	state.roll = conf.roll;
}

void ImGuiView::set_tracking_mode(bool is_tracking)
{
	tracking = is_tracking;
	config_visible = config_visible && !tracking;
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
	state = conf;
	syncBuffersFromState();
	registerTrackingShortcut(state.tracking_shortcut_enabled);
}

void ImGuiView::set_enabled(bool enabled)
{
	this->enabled = enabled;
}

void ImGuiView::set_visible(bool visible)
{
	running = visible;
}

void ImGuiView::show_message(const char* msg, MSG_SEVERITY severity)
{
	message_text = msg;
	message_severity = severity;
	message_open = true;
}

void ImGuiView::set_shortcuts(bool enabled)
{
	registerTrackingShortcut(enabled);
}

IView* ImGuiView::get_calibration_window()
{
	return this;
}

void ImGuiView::paint_video_frame(cv::Mat& img)
{
	std::lock_guard<std::mutex> lock(frame_mutex);
	if (img.channels() == 3)
		cv::cvtColor(img, pending_frame, cv::COLOR_RGB2RGBA);
	else
		img.copyTo(pending_frame);
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

void ImGuiView::cleanupDeviceD3D()
{
	cleanupRenderTarget();
	swap_chain.Reset();
	d3d_context.Reset();
	d3d_device.Reset();
}

void ImGuiView::createRenderTarget()
{
	Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
	swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
	d3d_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &render_target_view);
}

void ImGuiView::cleanupRenderTarget()
{
	render_target_view.Reset();
}

void ImGuiView::uploadPendingFrame()
{
	cv::Mat frame;
	{
		std::lock_guard<std::mutex> lock(frame_mutex);
		if (!frame_dirty || pending_frame.empty())
			return;
		pending_frame.copyTo(frame);
		frame_dirty = false;
	}

	if (frame.cols != texture_width || frame.rows != texture_height || !video_texture)
	{
		releaseVideoTexture();
		texture_width = frame.cols;
		texture_height = frame.rows;

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = texture_width;
		desc.Height = texture_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		d3d_device->CreateTexture2D(&desc, nullptr, &video_texture);
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = desc.Format;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;
		d3d_device->CreateShaderResourceView(video_texture.Get(), &srv_desc, &video_texture_view);
	}

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(d3d_context->Map(video_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		for (int y = 0; y < frame.rows; ++y)
			memcpy(static_cast<unsigned char*>(mapped.pData) + mapped.RowPitch * y, frame.ptr(y), frame.cols * 4);
		d3d_context->Unmap(video_texture.Get(), 0);
	}
}

void ImGuiView::releaseVideoTexture()
{
	video_texture_view.Reset();
	video_texture.Reset();
	texture_width = 0;
	texture_height = 0;
}

void ImGuiView::renderMainWindow()
{
	ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(420, state.show_video_feed ? 459 : 174), ImGuiCond_Always);
	ImGui::Begin("AITrack " AITRACK_VERSION, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

	if (state.show_video_feed)
		renderVideoPanel(video_texture_view.Get(), tracking ? "Waiting for camera frame..." : "Tracking stopped");

	ImGui::BeginDisabled(!enabled);
	if (ImGui::Button(tracking ? "Stop tracking" : "Start tracking", ImVec2(-1, 40)) && presenter)
		presenter->toggle_tracking();
	ImGui::EndDisabled();

	if (ImGui::Checkbox("Enable preview", &state.show_video_feed))
		applyPrefs();

	ImGui::BeginDisabled(tracking);
	if (ImGui::Button("Configuration", ImVec2(-1, 30)))
		config_visible = true;
	ImGui::EndDisabled();

	ImGui::End();
}

void ImGuiView::renderConfigWindow()
{
	if (!config_visible)
		return;

	ImGui::SetNextWindowSize(ImVec2(411, 401), ImGuiCond_Always);
	if (!ImGui::Begin("ConfigWindow", &config_visible, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		return;
	}

	ImGui::BeginDisabled(!enabled || tracking);
	ImGui::BeginChild("Camera", ImVec2(191, 331), true);
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
	ImGui::InputInt("Width", &state.video_width);
	ImGui::InputInt("Height", &state.video_height);
	ImGui::InputInt("FPS", &state.video_fps);
	ImGui::Separator();
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
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginGroup();
	ImGui::BeginChild("Remote", ImVec2(191, 71), true);
	ImGui::TextUnformatted("Use remote OpenTrack client");
	ImGui::InputText("IP", ip_buffer.data(), ip_buffer.size());
	ImGui::InputText("Port", port_buffer.data(), port_buffer.size(), ImGuiInputTextFlags_CharsDecimal);
	ImGui::EndChild();

	ImGui::BeginChild("Tracker parameters", ImVec2(191, 181), true);
	ImGui::TextUnformatted("Tracker parameters");
	ImGui::InputText("Distance (m)", distance_buffer.data(), distance_buffer.size(), ImGuiInputTextFlags_CharsDecimal);
	ImGui::InputText("Camera FOV", fov_buffer.data(), fov_buffer.size(), ImGuiInputTextFlags_CharsDecimal);
	const char* current_model = state.model_names.empty() ? "" : state.model_names[state.selected_model].c_str();
	if (ImGui::BeginCombo("Model type", current_model))
	{
		for (int i = 0; i < static_cast<int>(state.model_names.size()); ++i)
		{
			if (ImGui::Selectable(state.model_names[i].c_str(), state.selected_model == i))
				state.selected_model = i;
		}
		ImGui::EndCombo();
	}
	ImGui::Checkbox("Landmark stabilization", &state.use_landmark_stab);
	if (ImGui::Button("Calibrate Face"))
		calibration_visible = true;
	ImGui::EndChild();

	ImGui::BeginChild("General", ImVec2(191, 71), true);
	ImGui::TextUnformatted("General");
	ImGui::Checkbox("Autocheck updates", &state.autocheck_updates);
	ImGui::Checkbox("Start/Stop Tracking shortcut", &state.tracking_shortcut_enabled);
	ImGui::EndChild();
	ImGui::EndGroup();

	if (ImGui::Button("Apply", ImVec2(-1, 31)))
		applyPrefs();
	ImGui::EndDisabled();
	ImGui::End();
}

void ImGuiView::renderCalibrationWindow()
{
	if (!calibration_visible)
		return;

	ImGui::SetNextWindowSize(ImVec2(488, 406), ImGuiCond_Always);
	if (!ImGui::Begin("Head Calibration", &calibration_visible, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		return;
	}

	renderVideoPanel(video_texture_view.Get(), "Look directly at the camera, click \"Calibrate\" and wait a few seconds");
	if (ImGui::Button("Calibrate", ImVec2(-1, 40)) && presenter)
	{
		applyPrefs();
		presenter->calibrate_face(*this);
	}
	ImGui::End();
}

void ImGuiView::renderVideoPanel(ID3D11ShaderResourceView* texture, const char* empty_text)
{
	ImVec2 size(400, 280);
	ImGui::BeginChild("cameraView", size, false, ImGuiWindowFlags_NoScrollbar);
	if (texture)
		ImGui::Image(reinterpret_cast<ImTextureID>(texture), size);
	else
	{
		ImGui::SetCursorPosY((size.y - ImGui::GetTextLineHeightWithSpacing()) * 0.5f);
		ImGui::SetCursorPosX(12.0f);
		ImGui::TextWrapped("%s", empty_text);
	}
	ImGui::EndChild();
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

void ImGuiView::applyPrefs()
{
	if (!presenter)
		return;
	syncStateFromBuffers();
	presenter->save_prefs(state);
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
	if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
		return true;

	ImGuiView* view = reinterpret_cast<ImGuiView*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (msg == WM_NCCREATE)
	{
		CREATESTRUCT* create = reinterpret_cast<CREATESTRUCT*>(lparam);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}

	switch (msg)
	{
	case WM_SIZE:
		if (view && view->d3d_device && wparam != SIZE_MINIMIZED)
		{
			view->cleanupRenderTarget();
			view->swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
			view->createRenderTarget();
		}
		return 0;
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
