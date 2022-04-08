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
#include "platformstreammf.h"
#include "platformcontextmf.h"


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

	hr = MFCreateAttributes(pAttributes.GetAddressOf(), 1);
	if (!SUCCEEDED(hr))
		return false;

	hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

	if (!SUCCEEDED(hr))
		return false;

	UINT32   cDevices;
	IMFActivate** ppDevices = nullptr;

	hr = MFEnumDeviceSources(pAttributes.Get(), &ppDevices, &cDevices);
	if (!SUCCEEDED(hr))
		return false;

	for (UINT32 index = 0; index < cDevices; ++index)
	{
		platformDeviceInfo* info = new platformDeviceInfo();

		WCHAR* deviceName = NULL;
		UINT32 deviceNameLength = 0;
		UINT32 deviceIdLength = 0;
		WCHAR* deviceId = NULL;

		hr = ppDevices[index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
			&deviceName, &deviceNameLength);

		if (SUCCEEDED(hr))
			info->m_name = wstringToString(deviceName);

		CoTaskMemFree(deviceName);

		hr = ppDevices[index]->GetAllocatedString(
			MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &deviceId,
			&deviceIdLength);
		if (SUCCEEDED(hr))
		{
			info->m_uniqueID = wstringToString(deviceId);
			info->m_devicePath = deviceId;
		}

		CoTaskMemFree(deviceId);


		LOG(LOG_INFO, "ID %d -> %s\n", index, info->m_name.c_str());

		ComPtr<IMFMediaSource> pMediaSource;
		ComPtr<IMFSourceReader> pSourceReader;

		hr = ppDevices[index]->ActivateObject(__uuidof(IMFMediaSource), (void**)pMediaSource.GetAddressOf());
		hr = MFCreateSourceReaderFromMediaSource(pMediaSource.Get(), NULL, pSourceReader.GetAddressOf());
		if (!SUCCEEDED(hr))
		{
			LOG(LOG_DEBUG, "MFCreateSourceReaderFromMediaSource failed (HRESULT = %08X)!\n", hr);
			continue;
		}

		LOG(LOG_DEBUG, "Enumerate native media type:\n");


		DWORD dwMediaTypeIndex = 0;
		GUID subtype = GUID_NULL;
		UINT32 width = 0u;
		UINT32 height = 0u;
		UINT32 frameRateNum = 0u;
		UINT32 frameRateDenom = 0U;

		while (SUCCEEDED(hr))
		{
			ComPtr<IMFMediaType> mediaFormat;
			hr = pSourceReader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, dwMediaTypeIndex++, mediaFormat.GetAddressOf());

			if (FAILED(hr))
				break;

			CapFormatInfo frameInfo = { 0 };

			if (SUCCEEDED(mediaFormat->GetGUID(MF_MT_SUBTYPE, &subtype)))
				frameInfo.fourcc = subtype.Data1;

			if (SUCCEEDED(MFGetAttributeSize(mediaFormat.Get(), MF_MT_FRAME_SIZE, &width, &height))) {
				frameInfo.width = width;
				frameInfo.height = height;

			}

			if (SUCCEEDED(MFGetAttributeRatio(mediaFormat.Get(), MF_MT_FRAME_RATE, &frameRateNum, &frameRateDenom))) {
				frameInfo.fps = frameRateDenom != 0 ? (uint32_t)(((double)frameRateNum) / ((double)frameRateDenom)) : 0;
			}

			info->m_formats.push_back(frameInfo);

			LOG(LOG_VERBOSE, "    Format ID[%d] %d x %d  %d fps FOURCC=%s\n",
				dwMediaTypeIndex - 1, frameInfo.width, frameInfo.height, 
				frameInfo.fps, fourCCToString(frameInfo.fourcc).c_str());
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


