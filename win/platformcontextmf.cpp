/*

	OpenPnp-Capture: a video capture subsystem.

	Windows platform/implementation specific structures and typedefs.

	The platform classes are also responsible for converting
	the frames into 24-bit per pixel RGB frames.

	Created by Niels Moseley on 7/6/17.
	Copyright (c) 2017 Niels Moseley.

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all
	copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	SOFTWARE.
*/

#include <vector>
#include <stdio.h>

#include "../common/logging.h"
#include "scopedcomptr.h"
#include "platformstreammf.h"
#include "platformcontextmf.h"

#ifdef _MSC_VER
#pragma warning(disable:4503)
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mf")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "Strmiids")
#pragma comment(lib, "Mfreadwrite")
#endif
//==================================================================================================



// a platform factory function needed by
// libmain.cpp
Context* createPlatformContext()
{
	return new PlatformContextMF();
}

PlatformContextMF::PlatformContextMF() : Context()
{
	HRESULT hr;
	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
	if (hr != S_OK)
	{
		// This might happen when another part of the program
		// as already called CoInitializeEx.
		// and we can carry on without problems... 
		//LOG(LOG_WARNING, "PlatformContext::CoInitializeEx failed (HRESULT = %08X)!\n", hr);
	}
	else
	{
		LOG(LOG_DEBUG, "PlatformContext created\n");
	}

	hr = MFStartup(MF_VERSION);
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_WARNING, "MFStartup failed (HRESULT = %08X)!\n", hr);
	}

	enumerateDevices();
}

PlatformContextMF::~PlatformContextMF()
{
	CoUninitialize();
}


bool PlatformContextMF::enumerateDevices()
{
	HRESULT hr = S_OK;
	ComPtr<IMFAttributes> pAttributes;

	hr = MFCreateAttributes(&pAttributes, 1);
	if (!SUCCEEDED(hr))
		return false;

	hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

	if (!SUCCEEDED(hr))
		return false;

	UINT32   cDevices;
	IMFActivate** ppDevices = NULL;

	hr = MFEnumDeviceSources(pAttributes.Get(), &ppDevices, &cDevices);
	if (!SUCCEEDED(hr))
		return false;

	for (UINT32 deviceIndex = 0; deviceIndex < cDevices; ++deviceIndex)
	{
		WCHAR* ppszName = nullptr;
		WCHAR* ppszDevicePath = nullptr;

		std::wstring deviceName;
		std::wstring devicePath;

		UINT32 cchLengh = 0;
		hr = ppDevices[deviceIndex]->GetAllocatedString(
			MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
			&ppszName,
			&cchLengh
		);

		if (ppszName)
		{
			deviceName = ppszName;
			CoTaskMemFree(ppszName);
		}

		hr = ppDevices[deviceIndex]->GetAllocatedString(
			MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
			&ppszDevicePath,
			&cchLengh
		);

		if (ppszDevicePath)
		{
			devicePath = ppszDevicePath;
			CoTaskMemFree(ppszDevicePath);
		}


		LOG(LOG_INFO, "ID %d -> %s\n", deviceIndex, wcharPtrToString(deviceName.c_str()).c_str());

		ComPtr<IMFMediaSource> pMediaSource;
		hr = ppDevices[deviceIndex]->ActivateObject(__uuidof(IMFMediaSource), (void**)&pMediaSource);
		if (!SUCCEEDED(hr))
		{
			LOG(LOG_DEBUG, "ActivateObject failed (HRESULT = %08X)!\n", hr);
			continue;
		}

		ComPtr<IMFSourceReader> pSourceReader;
		hr = MFCreateSourceReaderFromMediaSource(pMediaSource.Get(), NULL, &pSourceReader);
		if (!SUCCEEDED(hr))
		{
			LOG(LOG_DEBUG, "MFCreateSourceReaderFromMediaSource failed (HRESULT = %08X)!\n", hr);
			continue;
		}

		LOG(LOG_DEBUG, "Enumerate native media type:\n");

		platformDeviceInfo* info = new platformDeviceInfo();

		info->m_name = wcharPtrToString(deviceName.c_str());
		info->m_uniqueID = wcharPtrToString(devicePath.c_str());
		info->m_devicePath = devicePath;

		int index = 0;
		while (SUCCEEDED(hr))
		{
			ComPtr<IMFMediaType> raw_type;
			hr = pSourceReader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, index++, &raw_type);

			if (FAILED(hr))
				break;

			MediaType mediaType(raw_type.Get());
			if (mediaType.majorType == MFMediaType_Video)
			{
				CapFormatInfo frameInfo;
				frameInfo.width = mediaType.width;
				frameInfo.height = mediaType.height;
				frameInfo.fourcc = mediaType.subType.Data1;
				frameInfo.fps = (int)mediaType.getFramerate();
				//frameInfo.bpp

				info->m_formats.push_back(frameInfo);

				LOG(LOG_VERBOSE, "    Format ID[%d] %d x %d  %d fps FOURCC=%s\n", index - 1, frameInfo.width, frameInfo.height, frameInfo.fps, fourCCToString(frameInfo.fourcc).c_str());
			}
		}

		m_devices.push_back(info);
	}


	if (ppDevices)
	{
		for (UINT32 i = 0; i < cDevices; ++i)
			ppDevices[i]->Release();
		CoTaskMemFree(ppDevices);
	}

	return true;
}


std::string PlatformContextMF::wstringToString(const std::wstring& wstr)
{
	return wcharPtrToString(wstr.c_str());
}

std::string PlatformContextMF::wcharPtrToString(const wchar_t* sstr)
{
	std::vector<char> buffer;
	int32_t chars = WideCharToMultiByte(CP_UTF8, 0, sstr, -1, nullptr, 0, nullptr, nullptr);
	if (chars == 0) return std::string("");

	buffer.resize(chars);
	WideCharToMultiByte(CP_UTF8, 0, sstr, -1, &buffer[0], chars, nullptr, nullptr);
	return std::string(&buffer[0]);
}


