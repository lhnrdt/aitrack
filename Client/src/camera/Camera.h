#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "CameraSettings.h"

struct CameraVideoMode
{
	int width;
	int height;
	int fps;
};

class Camera
{
public:
	int width, height, fps;
	bool is_valid = false;

	virtual void start_camera() = 0;
	virtual void stop_camera() = 0;
	virtual void get_frame(uint8_t* buffer) = 0;
	virtual void set_settings(CameraSettings& settings) = 0;
	virtual CameraSettings get_settings() = 0;
	virtual void set_manual_exposure(int exposure) { (void)exposure; }
	virtual std::vector<int> get_available_fps() { return {}; }
	virtual std::vector<CameraVideoMode> get_available_video_modes() { return {}; }
	virtual std::string get_name() const { return "Camera"; }

	Camera(int width, int height, int fps) {
		this->width = width;
		this->height = height;
		this->fps = fps;
	}
};