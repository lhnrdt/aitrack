#include "Config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace
{
	std::string trim(const std::string& value)
	{
		auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); });
		auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
		if (first >= last)
			return "";
		return std::string(first, last);
	}

	std::string boolToString(bool value)
	{
		return value ? "true" : "false";
	}
}



ConfigData ConfigData::getGenericConfig()
{
	ConfigData conf = ConfigData();
	conf.ip = "";
	conf.port = 0;
	conf.camera_fov = 56.0;
	conf.prior_distance = .7;
	conf.show_video_feed = true;
	conf.show_diagnostics = false;
	conf.selected_model = 0;
	conf.selected_camera = 0;
	conf.num_cameras_detected = 0;
	conf.video_width = -1;
	conf.video_height = -1;
	conf.video_fps = 30;
	conf.use_landmark_stab = true;
	conf.autocheck_updates = true;
	conf.tracking_shortcut_enabled = false;
	conf.dark_mode = false;
	conf.x = conf.y = conf.z = conf.pitch = conf.yaw = conf.roll = 0.0f;
	conf.cam_exposure = -1;
	conf.cam_gain = -1;
	conf.onnx_set_env_threads = true;
	conf.onnx_env_threads = 1;
	conf.onnx_set_num_threads = true;
	conf.onnx_num_threads = 1;
	conf.onnx_set_dynamic = true;
	conf.onnx_dynamic = 0;
	conf.head_scale_x = 1.0;
	//conf.head_scale_y = 1.0;
	//conf.head_scale_z = 1.0;

	return conf;
}


ConfigMgr::ConfigMgr(std::string ini_path):
	ini_path(std::move(ini_path))
{
	load();
	if(values.empty())
	{
		ConfigData cnf_default = ConfigData::getGenericConfig();
		updateConfig(cnf_default);
	}
}

void ConfigMgr::load()
{
	values.clear();

	std::ifstream file(ini_path);
	if (!file.is_open())
		return;

	std::string line;
	while (std::getline(file, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
			continue;

		const std::size_t delimiter = line.find('=');
		if (delimiter == std::string::npos)
			continue;

		values[trim(line.substr(0, delimiter))] = trim(line.substr(delimiter + 1));
	}
}

std::string ConfigMgr::getValue(const std::string& key, const std::string& default_value) const
{
	auto iter = values.find(key);
	if (iter == values.end())
		return default_value;
	return iter->second;
}

int ConfigMgr::getInt(const std::string& key, int default_value) const
{
	try {
		return std::stoi(getValue(key, std::to_string(default_value)));
	}
	catch (...) {
		return default_value;
	}
}

double ConfigMgr::getDouble(const std::string& key, double default_value) const
{
	try {
		return std::stod(getValue(key, std::to_string(default_value)));
	}
	catch (...) {
		return default_value;
	}
}

bool ConfigMgr::getBool(const std::string& key, bool default_value) const
{
	std::string value = getValue(key, boolToString(default_value));
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value == "true" || value == "1" || value == "yes";
}

void ConfigMgr::updateConfig(const ConfigData& data)
{
	values["ip"] = data.ip;
	values["port"] = std::to_string(data.port);
	values["camera_fov"] = std::to_string(data.camera_fov);
	values["prior_distance"] = std::to_string(data.prior_distance);
	values["video_feed"] = boolToString(data.show_video_feed);
	values["diagnostics"] = boolToString(data.show_diagnostics);
	values["model"] = std::to_string(data.selected_model);
	values["video_width"] = std::to_string(data.video_width);
	values["video_height"] = std::to_string(data.video_height);
	values["stabilize_landmarks"] = boolToString(data.use_landmark_stab);
	values["fps"] = std::to_string(data.video_fps);
	values["cam_exposure"] = std::to_string(data.cam_exposure);
	values["cam_gain"] = std::to_string(data.cam_gain);
	values["selected_camera"] = std::to_string(data.selected_camera);
	values["autocheck_updates"] = boolToString(data.autocheck_updates);
	values["dark_mode"] = boolToString(data.dark_mode);

	values["set_env_threads"] = boolToString(data.onnx_set_env_threads);
	values["env_threads"] = std::to_string(data.onnx_env_threads);
	values["set_num_threads"] = boolToString(data.onnx_set_num_threads);
	values["num_threads"] = std::to_string(data.onnx_num_threads);
	values["set_dynamic"] = boolToString(data.onnx_set_dynamic);
	values["dynamic"] = std::to_string(data.onnx_dynamic);

	values["head_3d_scale_x"] = std::to_string(data.head_scale_x);
	values["tracking_shortcut_enabled"] = boolToString(data.tracking_shortcut_enabled);

	std::ofstream file(ini_path, std::ios::trunc);
	for (const auto& value : values)
		file << value.first << "=" << value.second << "\n";

}

ConfigData ConfigMgr::getConfig()
{
	ConfigData c = ConfigData();
	c.ip = getValue("ip", "");
	c.port = getInt("port", 0);
	c.camera_fov = getDouble("camera_fov", 56.0);
	c.prior_distance = getDouble("prior_distance", 0.0);
	c.show_video_feed = getBool("video_feed", true);
	c.show_diagnostics = getBool("diagnostics", false);
	c.use_landmark_stab = getBool("stabilize_landmarks", true);
	c.selected_model = getInt("model", 0);
	c.selected_camera = getInt("selected_camera", 0);
	c.video_width = getInt("video_width", 640);
	c.video_height = getInt("video_height", 480);
	c.video_fps = getInt("fps", 30);
	c.cam_exposure= getInt("cam_exposure", -1);
	c.cam_gain = getInt("cam_gain", -1);
	c.autocheck_updates = getBool("autocheck_updates", true);
	c.dark_mode = getBool("dark_mode", false);
	c.onnx_set_env_threads = getBool("set_env_threads", true);
	c.onnx_env_threads = getInt("env_threads", 1);
	c.onnx_set_num_threads = getBool("set_num_threads", true);
	c.onnx_num_threads = getInt("num_threads", 1);
	c.onnx_set_dynamic = getBool("set_dynamic", true);
	c.onnx_dynamic = getInt("dynamic", 0);
	c.head_scale_x = getDouble("head_3d_scale_x", 1.0);
	//c.head_scale_y = conf.value("head_3d_scale_y", 1.0).toDouble();
	//c.head_scale_z = conf.value("head_3d_scale_z", 1.0).toDouble();
	c.tracking_shortcut_enabled= getBool("tracking_shortcut_enabled", false);

	return c;
}

