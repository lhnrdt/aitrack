#pragma once

#include <string>
#include <map>
#include <vector>

#include "../camera/Camera.h"


/**
* Struct which holds both the program state and the serializable part of this
* state (aka. preferences).
*/
struct ConfigData
{
	std::string ip;
	int port;
	int video_height;
	int video_width;
	int video_fps;
	std::vector<int> available_fps;
	std::vector<CameraVideoMode> available_video_modes;
	std::vector<std::string> available_camera_names;
	double prior_distance, camera_fov;
	bool show_video_feed;
	bool show_diagnostics;
	bool expert_mode;
	bool use_landmark_stab;
	bool autocheck_updates;
	bool tracking_shortcut_enabled;
	bool dark_mode;

	float x, y, z, yaw, pitch, roll;

	double head_scale_x, head_scale_y, head_scale_z;

	int selected_camera;
	int num_cameras_detected;
	int cam_exposure;
	int cam_gain;
	bool face_auto_exposure;

	bool onnx_set_env_threads;
	int onnx_env_threads;
	bool onnx_set_num_threads;
	int onnx_num_threads;
	bool onnx_set_dynamic;
	int onnx_dynamic;

	std::vector<std::string> model_names;
	int selected_model;

	static ConfigData getGenericConfig();
};


/**
* Incharged of building/retrieving/saving a config object.
*/
class ConfigMgr
{
public:
	ConfigMgr(std::string ini_path);
	void updateConfig(const ConfigData& data);
	ConfigData getConfig();

private:
	std::string ini_path;
	std::map<std::string, std::string> values;

	void load();
	std::string getValue(const std::string& key, const std::string& default_value) const;
	int getInt(const std::string& key, int default_value) const;
	double getDouble(const std::string& key, double default_value) const;
	bool getBool(const std::string& key, bool default_value) const;
};
