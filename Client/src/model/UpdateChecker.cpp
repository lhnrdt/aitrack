#include "UpdateChecker.h"

#include <iostream>
#include <thread>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winhttp.h>

namespace
{
	std::wstring widen(const std::string& value)
	{
		if (value.empty())
			return L"";

		int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
		std::wstring result(size - 1, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &result[0], size);
		return result;
	}

	std::string readUrl(const std::wstring& host, const std::wstring& path)
	{
		std::string response;
		HINTERNET session = WinHttpOpen(L"AITrack/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session)
			return response;

		HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
		HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
		if (request)
		{
			const wchar_t* headers = L"User-Agent: AITrack\r\nAccept: application/vnd.github.v3+json\r\n";
			if (WinHttpSendRequest(request, headers, -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr))
			{
				DWORD available = 0;
				while (WinHttpQueryDataAvailable(request, &available) && available > 0)
				{
					std::string chunk(available, '\0');
					DWORD read = 0;
					if (!WinHttpReadData(request, &chunk[0], available, &read) || read == 0)
						break;
					chunk.resize(read);
					response += chunk;
				}
			}
		}

		if (request)
			WinHttpCloseHandle(request);
		if (connection)
			WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return response;
	}

	std::string parseLatestTag(const std::string& json)
	{
		const std::string key = "\"tag_name\"";
		std::size_t key_pos = json.find(key);
		if (key_pos == std::string::npos)
			return "";

		std::size_t colon = json.find(':', key_pos + key.size());
		std::size_t first_quote = json.find('"', colon);
		std::size_t second_quote = json.find('"', first_quote + 1);
		if (colon == std::string::npos || first_quote == std::string::npos || second_quote == std::string::npos)
			return "";

		return json.substr(first_quote + 1, second_quote - first_quote - 1);
	}
}


UpdateChecker::UpdateChecker(std::string& version, IUpdateSub *obs):
    current_version(version)
{
    this->observer = obs;
}

void UpdateChecker::get_latest_update(std::string& repo)
{
	std::thread([this, repo]() {
		std::string json = readUrl(L"api.github.com", L"/repos/" + widen(repo) + L"/releases");
		std::string tag = parseLatestTag(json);
		if (tag.empty())
			return;

		Version v(tag);
		this->observer->on_update_check_completed((current_version < v));
	}).detach();
}
