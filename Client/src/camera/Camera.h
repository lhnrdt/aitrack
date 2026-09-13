#pragma once

#include <cstdint>
#include <vector>
#include "CameraSettings.h"

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
	virtual std::vector<int> get_available_fps() { return {}; }

	Camera(int width, int height, int fps) {
		this->width = width;
		this->height = height;
		this->fps = fps;
	}
};