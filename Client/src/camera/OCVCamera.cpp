#include "OCVCamera.h"

#include <cmath>
#include <dshow.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "strmiids.lib")

namespace
{
	std::string getDirectShowCameraName(int camera_index)
	{
		IMoniker* moniker = nullptr;
		IEnumMoniker* enumerator = nullptr;
		ICreateDevEnum* device_enumerator = nullptr;
		std::string name;
		const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		const bool initialized_com = SUCCEEDED(com_result);

		if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
			IID_ICreateDevEnum, reinterpret_cast<void**>(&device_enumerator))))
		{
			if (initialized_com)
				CoUninitialize();
			return name;
		}
		if (FAILED(device_enumerator->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumerator, 0)) || !enumerator)
		{
			device_enumerator->Release();
			if (initialized_com)
				CoUninitialize();
			return name;
		}

		int current_index = 0;
		while (enumerator->Next(1, &moniker, nullptr) == S_OK)
		{
			if (current_index++ == camera_index)
			{
				IPropertyBag* properties = nullptr;
				if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag,
					reinterpret_cast<void**>(&properties))))
				{
					VARIANT value;
					VariantInit(&value);
					if (SUCCEEDED(properties->Read(L"FriendlyName", &value, nullptr)) && value.vt == VT_BSTR)
					{
						int length = WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, -1, nullptr, 0, nullptr, nullptr);
						std::string utf8(length, '\0');
						WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, -1, &utf8[0], length, nullptr, nullptr);
						utf8.pop_back();
						name = utf8;
					}
					VariantClear(&value);
					properties->Release();
				}
				moniker->Release();
				break;
			}
			moniker->Release();
			moniker = nullptr;
		}
		enumerator->Release();
		device_enumerator->Release();
		if (initialized_com)
			CoUninitialize();
		return name;
	}

	void releaseMediaType(AM_MEDIA_TYPE* media_type)
	{
		if (!media_type)
			return;
		if (media_type->cbFormat && media_type->pbFormat)
			CoTaskMemFree(media_type->pbFormat);
		if (media_type->pUnk)
			media_type->pUnk->Release();
		CoTaskMemFree(media_type);
	}

	std::vector<CameraVideoMode> getDirectShowCameraModes(int camera_index)
	{
		std::vector<CameraVideoMode> available;
		const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		const bool initialized_com = SUCCEEDED(com_result);
		ICreateDevEnum* device_enumerator = nullptr;
		IEnumMoniker* enumerator = nullptr;
		IMoniker* moniker = nullptr;
		IBaseFilter* filter = nullptr;
		IEnumPins* pins = nullptr;
		IPin* pin = nullptr;

		if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
			IID_ICreateDevEnum, reinterpret_cast<void**>(&device_enumerator))) ||
			FAILED(device_enumerator->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumerator, 0)) ||
			!enumerator)
			goto cleanup;

		for (int current_index = 0; current_index <= camera_index; ++current_index)
		{
			if (enumerator->Next(1, &moniker, nullptr) != S_OK)
				goto cleanup;
			if (current_index == camera_index)
				break;
			moniker->Release();
			moniker = nullptr;
		}

		if (FAILED(moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, reinterpret_cast<void**>(&filter))) ||
			FAILED(filter->EnumPins(&pins)))
			goto cleanup;

		while (pins->Next(1, &pin, nullptr) == S_OK)
		{
			PIN_INFO pin_info{};
			if (SUCCEEDED(pin->QueryPinInfo(&pin_info)) && pin_info.dir == PINDIR_OUTPUT)
			{
				IAMStreamConfig* config = nullptr;
				if (SUCCEEDED(pin->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&config))))
				{
					int capability_count = 0;
					int capability_size = 0;
					if (SUCCEEDED(config->GetNumberOfCapabilities(&capability_count, &capability_size)))
					{
						for (int i = 0; i < capability_count; ++i)
						{
							std::vector<BYTE> capability(static_cast<size_t>(capability_size));
							AM_MEDIA_TYPE* media_type = nullptr;
							if (SUCCEEDED(config->GetStreamCaps(i, &media_type, capability.data())) &&
								media_type && media_type->formattype == FORMAT_VideoInfo &&
								media_type->pbFormat)
							{
								const VIDEOINFOHEADER* video_info = reinterpret_cast<const VIDEOINFOHEADER*>(media_type->pbFormat);
								if (video_info->AvgTimePerFrame > 0)
								{
									available.push_back({
										video_info->bmiHeader.biWidth,
										std::abs(video_info->bmiHeader.biHeight),
										static_cast<int>((10000000LL + video_info->AvgTimePerFrame / 2) /
											video_info->AvgTimePerFrame)
									});
								}
							}
							releaseMediaType(media_type);
						}
					}
					config->Release();
				}
			}
			if (pin_info.pFilter)
				pin_info.pFilter->Release();
			pin->Release();
			pin = nullptr;
		}

	cleanup:
		if (pins) pins->Release();
		if (filter) filter->Release();
		if (moniker) moniker->Release();
		if (enumerator) enumerator->Release();
		if (device_enumerator) device_enumerator->Release();
		if (initialized_com) CoUninitialize();
		std::sort(available.begin(), available.end(), [](const CameraVideoMode& left, const CameraVideoMode& right) {
			if (left.width != right.width) return left.width < right.width;
			if (left.height != right.height) return left.height < right.height;
			return left.fps < right.fps;
		});
		available.erase(std::unique(available.begin(), available.end(), [](const CameraVideoMode& left, const CameraVideoMode& right) {
			return left.width == right.width && left.height == right.height && left.fps == right.fps;
		}), available.end());
		return available;
	}
}

OCVCamera::OCVCamera(int width, int height, int fps, int index) :
	Camera(width, height, fps),
	size(0, 0),
	cap(),
	cam_index(index)
{
	CV_BACKEND = cv::CAP_DSHOW;
	if (!is_camera_available())
	{
		// Check again with Media foundation backend
		CV_BACKEND = cv::CAP_MSMF;
		if (!is_camera_available())
			throw std::runtime_error("No compatible camera found.");
	}
	is_valid = true;


	if (width < 0 || height < 0)
	{
		this->width = cam_native_width;
		this->height = cam_native_height;
	}
	
	if (fps <= 0)
		this->fps = cam_native_fps;
	camera_name = getDirectShowCameraName(cam_index);
	if (camera_name.empty())
		camera_name = "Camera " + std::to_string(cam_index);

	exposure, gain = -1;
}

OCVCamera::~OCVCamera()
{
	stop_camera();
}

bool OCVCamera::is_camera_available()
{
	bool available = false;

	cap.open(cam_index, CV_BACKEND);
	available = cap.isOpened();
	if (available)
	{
		cv::Mat frame;
		cap.read(frame);
		if (frame.empty())
			return false;

		cam_native_width = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
		cam_native_height = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
		const int detected_fps = static_cast<int>(cap.get(cv::CAP_PROP_FPS));
		cam_native_fps = detected_fps > 30 ? detected_fps : 30;
		cap.release();
	}
	return available;
}

void OCVCamera::start_camera()
{
	cap.open(cam_index, CV_BACKEND);
	if (!cap.isOpened())
	{
		throw std::runtime_error("No compatible camera found.");
	}

	// Force its properties each time we start the camera
	// because if we force them with the device switched off
	// bugs will occur (tiling, for example).
	if (cap.get(cv::CAP_PROP_FRAME_WIDTH) != this->width)
		cap.set(cv::CAP_PROP_FRAME_WIDTH, this->width);
	if (cap.get(cv::CAP_PROP_FRAME_HEIGHT) != this->height)
		cap.set(cv::CAP_PROP_FRAME_HEIGHT, this->height);
	if (cap.get(cv::CAP_PROP_FPS) != this->fps)
		cap.set(cv::CAP_PROP_FPS, this->fps);
}

void OCVCamera::stop_camera()
{
	cap.release();
}

void OCVCamera::get_frame(uint8_t* buffer)
{
	cv::Mat frame;
	cap.read(frame);
	cv::flip(frame, frame, 1);
	for (int i = 0; i < frame.cols * frame.rows * 3; i++)
		buffer[i] = frame.data[i];

}

void OCVCamera::set_settings(CameraSettings& settings)
{
	this->width = settings.width > 0 ? settings.width : this->cam_native_width;
	this->height = settings.height > 0 ? settings.height : this->cam_native_height;
	this->fps = settings.fps > 0 ? settings.fps : this->cam_native_fps;

	// Disabled for the moment because of the different ranges in generic cameras.
	//exposure = settings.exposure < 0 ? -1.0F : (float)settings.exposure/255;
	//gain = settings.gain < 0 ? -1.0F : (float)settings.gain / 64;
	//cap.set(cv::CAP_PROP_EXPOSURE, exposure);
	//cap.set(cv::CAP_PROP_GAIN, gain);
}

CameraSettings OCVCamera::get_settings()
{
	return CameraSettings();
}

std::vector<int> OCVCamera::get_available_fps()
{
	std::vector<int> available;
	for (const CameraVideoMode& mode : getDirectShowCameraModes(cam_index))
		if (mode.width == width && mode.height == height)
			available.push_back(mode.fps);
	return available;
}

std::vector<CameraVideoMode> OCVCamera::get_available_video_modes()
{
	return getDirectShowCameraModes(cam_index);
}

std::string OCVCamera::get_name() const
{
	return camera_name;
}
