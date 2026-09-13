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
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct ImGuiContext;

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
	bool createSwapChainForWindow(HWND target_hwnd, Microsoft::WRL::ComPtr<IDXGISwapChain>& target_swap_chain);
	void cleanupDeviceD3D();
	void createRenderTarget();
	void createRenderTarget(IDXGISwapChain* target_swap_chain, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& target_render_target_view);
	void cleanupRenderTarget();
	bool createConfigWindow();
	bool createCalibrationWindow();
	void destroyConfigWindow();
	void destroyCalibrationWindow();
	void uploadPendingFrame();
	void uploadFrame(cv::Mat& frame, Microsoft::WRL::ComPtr<ID3D11Texture2D>& target_texture,
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& target_texture_view,
		int& target_texture_width, int& target_texture_height);
	void releaseVideoTexture();
	void releaseCalibrationTexture();
	void resizeHostWindowForMainContent(bool force = false);
	void renderMainWindow();
	void renderConfigWindow();
	void renderConfigContent();
	void renderCalibrationWindow();
	void renderVideoPanel(ID3D11ShaderResourceView* texture, const char* empty_text);
	void syncBuffersFromState();
	void syncStateFromBuffers();
	void applyPrefs();
	void processPendingUiRequests();
	void joinCompletedOperationThreads();
	void stopOperationThreads();
	void startTrackingOperation();
	void startApplyOperation();
	void startCalibrationOperation();
	void renderSpinner(const char* label);
	void renderSpinnerOnLastItem();
	bool isUiThread() const;
	void queueTrackingMode(bool is_tracking);
	void queueViewState(ConfigData conf);
	void queueEnabled(bool enabled);
	void queueVisible(bool visible);
	void queueMessage(std::string msg, MSG_SEVERITY severity);
	void applyCurrentTheme();
	void applyThemeForCurrentContext();
	void registerTrackingShortcut(bool enabled);

	static LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
	static LRESULT WINAPI configWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
	static LRESULT WINAPI calibrationWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	IPresenter* presenter = nullptr;
	DWORD ui_thread_id = 0;
	ImGuiContext* main_context = nullptr;
	ImGuiContext* config_context = nullptr;
	ImGuiContext* calibration_context = nullptr;
	HWND hwnd = nullptr;
	HWND config_hwnd = nullptr;
	HWND calibration_hwnd = nullptr;
	bool running = true;
	bool enabled = true;
	bool tracking = false;
	bool config_visible = false;
	bool calibration_visible = false;
	bool shortcut_enabled = false;
	bool host_show_video_feed = false;
	std::atomic<bool> tracking_operation{ false };
	std::atomic<bool> apply_operation{ false };
	std::atomic<bool> calibration_operation{ false };

	ConfigData state = ConfigData::getGenericConfig();

	std::thread tracking_operation_thread;
	std::thread apply_operation_thread;
	std::thread calibration_operation_thread;

	std::mutex pending_ui_mutex;
	bool has_pending_tracking_mode = false;
	bool pending_tracking_mode = false;
	bool has_pending_tracking_data = false;
	ConfigData pending_tracking_data = ConfigData::getGenericConfig();
	bool has_pending_view_state = false;
	ConfigData pending_view_state = ConfigData::getGenericConfig();
	bool has_pending_enabled = false;
	bool pending_enabled = true;
	bool has_pending_visible = false;
	bool pending_visible = true;
	bool has_pending_shortcuts = false;
	bool pending_shortcuts = false;
	bool has_pending_message = false;
	std::string pending_message;
	MSG_SEVERITY pending_message_severity = NORMAL;

	std::array<char, 64> ip_buffer{};
	std::array<char, 16> port_buffer{};
	std::array<char, 32> distance_buffer{};
	std::array<char, 32> fov_buffer{};

	std::mutex frame_mutex;
	cv::Mat pending_frame;
	cv::Mat calibration_pending_frame;
	bool frame_dirty = false;
	bool calibration_frame_dirty = false;
	int texture_width = 0;
	int texture_height = 0;
	int calibration_texture_width = 0;
	int calibration_texture_height = 0;

	Microsoft::WRL::ComPtr<ID3D11Device> d3d_device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context;
	Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain;
	Microsoft::WRL::ComPtr<IDXGISwapChain> config_swap_chain;
	Microsoft::WRL::ComPtr<IDXGISwapChain> calibration_swap_chain;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_view;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> config_render_target_view;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> calibration_render_target_view;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> video_texture;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> video_texture_view;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> calibration_texture;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> calibration_texture_view;
};
