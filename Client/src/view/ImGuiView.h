#pragma once

#include "i_view.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <mutex>
#include <string>

class ImGuiView : public IRootView, public IView
{
public:
	ImGuiView();
	~ImGuiView();

	int run();

	void connect_presenter(IPresenter* presenter) override;
	void show_tracking_data(ConfigData conf) override;
	void set_tracking_mode(bool is_tracking) override;
	ConfigData get_inputs() override;
	void update_view_state(ConfigData conf) override;
	void set_enabled(bool enabled) override;
	void set_visible(bool visible) override;
	void show_message(const char* msg, MSG_SEVERITY severity) override;
	void set_shortcuts(bool enabled) override;
	IView* get_calibration_window() override;
	void paint_video_frame(cv::Mat& img) override;
	void notify(IView* self) override;

private:
	bool createDeviceD3D(HWND hwnd);
	void cleanupDeviceD3D();
	void createRenderTarget();
	void cleanupRenderTarget();
	void uploadPendingFrame();
	void releaseVideoTexture();
	void renderMainWindow();
	void renderConfigWindow();
	void renderCalibrationWindow();
	void renderVideoPanel(ID3D11ShaderResourceView* texture, const char* empty_text);
	void syncBuffersFromState();
	void syncStateFromBuffers();
	void applyPrefs();
	void registerTrackingShortcut(bool enabled);

	static LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	IPresenter* presenter = nullptr;
	HWND hwnd = nullptr;
	bool running = true;
	bool enabled = true;
	bool tracking = false;
	bool config_visible = false;
	bool calibration_visible = false;
	bool shortcut_enabled = false;

	ConfigData state = ConfigData::getGenericConfig();
	std::string message_text;
	MSG_SEVERITY message_severity = NORMAL;
	bool message_open = false;

	std::array<char, 64> ip_buffer{};
	std::array<char, 16> port_buffer{};
	std::array<char, 32> distance_buffer{};
	std::array<char, 32> fov_buffer{};

	std::mutex frame_mutex;
	cv::Mat pending_frame;
	bool frame_dirty = false;
	int texture_width = 0;
	int texture_height = 0;

	Microsoft::WRL::ComPtr<ID3D11Device> d3d_device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context;
	Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_view;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> video_texture;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> video_texture_view;
};
