/*

	OpenPnp-Capture: a video capture subsystem.

	Windows Stream class

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

#include "platformdeviceinfo.h"
#include "platformstreammf.h"
#include "platformcontextmf.h"
#include "scopedcomptr.h"
#include <shlwapi.h>  // QISearch


#ifdef _MSC_VER
#pragma comment(lib, "shlwapi.lib")
#endif



/** convert a FOURCC uint32_t to human readable form */
std::string fourCCToStringWin(uint32_t fourcc)
{
	if (fourcc == 20)
		return std::string("RGB24");
	else if (fourcc == 21)
		return std::string("ARGB32");
	else if (fourcc == 22)
		return std::string("RGB32");

	std::string v;
	for (uint32_t i = 0; i < 4; i++)
	{
		v += static_cast<char>(fourcc & 0xFF);
		fourcc >>= 8;
	}
	return v;
};


Stream* createPlatformStream()
{
	return new PlatformStreamMF();
}

// **********************************************************************
//   Property translation data
// **********************************************************************

struct property_t
{
	uint32_t dsProp;            // Directshow CameraControlProperty or VideoProcAmpProperty
	bool     isCameraControl;   // if true dsProp is CameraControlProperty
};

// the order must be the same as the CAPPROPID indeces!
static const property_t gs_properties[] =
{
	{0, true},                      // dummy
	{CameraControl_Exposure, true}, // exposure
	{CameraControl_Focus, true},
	{CameraControl_Zoom, true},
	{VideoProcAmp_WhiteBalance, false},
	{VideoProcAmp_Gain, false},
	{VideoProcAmp_Brightness, false},
	{VideoProcAmp_Contrast, false},
	{VideoProcAmp_Saturation, false},
	{VideoProcAmp_Gamma, false},
	{VideoProcAmp_Hue, false},
	{VideoProcAmp_Sharpness, false},
	{VideoProcAmp_BacklightCompensation, false}
};


// **********************************************************************
//   PlatformStream
// **********************************************************************

PlatformStreamMF::PlatformStreamMF() :
	Stream(),
	m_ReaderCB(nullptr),
	m_pMediaSource(nullptr),
	m_pSourceReader(nullptr),
	m_camControl(nullptr),
	m_videoProcAmp(nullptr),
	m_bCapture(false)
{

}

PlatformStreamMF::~PlatformStreamMF()
{
	close();
}

void PlatformStreamMF::close()
{
	LOG(LOG_INFO, "closing stream\n");

#ifdef USE_SOURCE_READER_ASYNC_CALLBACK

	if (m_ReaderCB && m_bCapture)
	{
		HANDLE flushed = CreateEvent(NULL, TRUE, FALSE, NULL);
		const int kFlushTimeOutInMs = 1000;
		bool wait = false;

		m_bCapture = false;
		m_ReaderCB->SetSignalOnFlush(&flushed);
		wait = SUCCEEDED(m_pSourceReader->Flush(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS)));
		if (!wait) {
			m_ReaderCB->SetSignalOnFlush(NULL);
		}

		// If the device has been unplugged, the Flush() won't trigger the event
		// and a timeout will happen.
		// TODO(tommi): Hook up the IMFMediaEventGenerator notifications API and
		// do not wait at all after getting MEVideoCaptureDeviceRemoved event.
		// See issue/226396.
		if (wait)
			WaitForSingleObject(flushed, kFlushTimeOutInMs);

		SafeRelease(&m_ReaderCB);
	}
#else
	m_bCapture = false;
	if (m_readThread.joinable())
	{
		if (std::this_thread::get_id() != m_readThread.get_id())
			m_readThread.join();
		else
			m_readThread.detach();
	}
#endif

	SafeRelease(&m_camControl);
	SafeRelease(&m_videoProcAmp);
	SafeRelease(&m_pSourceReader);
	SafeRelease(&m_pMediaSource);

	m_owner = nullptr;
	m_width = 0;
	m_height = 0;
	m_frameBuffer.resize(0);
	m_isOpen = false;
}

static HRESULT findCaptureDevice(const wchar_t* devicePath, ComPtr<IMFActivate>& pActivate)
{
	ComPtr<IMFAttributes> pAttributes;

	HRESULT hr = S_OK;
	hr = MFCreateAttributes(&pAttributes, 1);
	if (!SUCCEEDED(hr))
		return hr;

	hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

	if (!SUCCEEDED(hr))
		return hr;

	UINT32   cDevices;
	IMFActivate** ppDevices = NULL;

	hr = MFEnumDeviceSources(pAttributes.Get(), &ppDevices, &cDevices);
	if (!SUCCEEDED(hr))
		return hr;

	for (UINT32 deviceIndex = 0; deviceIndex < cDevices; ++deviceIndex)
	{
		WCHAR* ppszName = nullptr;
		WCHAR* ppszDevicePath = nullptr;

		std::wstring strDeviceName;
		std::wstring strDevicePath;

		UINT32 cchLengh = 0;
		hr = ppDevices[deviceIndex]->GetAllocatedString(
			MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
			&ppszName,
			&cchLengh
		);

		if (ppszName)
		{
			strDeviceName = ppszName;
			CoTaskMemFree(ppszName);
		}

		hr = ppDevices[deviceIndex]->GetAllocatedString(
			MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
			&ppszDevicePath,
			&cchLengh
		);

		if (ppszDevicePath)
		{
			strDevicePath = ppszDevicePath;
			CoTaskMemFree(ppszDevicePath);
		}

		if (devicePath == strDevicePath)
		{
			pActivate = ppDevices[deviceIndex];
			break;
		}
	}

	if (ppDevices)
	{
		for (UINT32 i = 0; i < cDevices; ++i)
			ppDevices[i]->Release();
		CoTaskMemFree(ppDevices);
	}

	return S_OK;
}

bool PlatformStreamMF::open(Context* owner, deviceInfo* device, uint32_t width, uint32_t height,
	uint32_t fourCC, uint32_t fps)
{
	if (m_isOpen)
	{
		LOG(LOG_INFO, "open() was called on an active stream.\n");
		close();
	}

	if (owner == nullptr)
	{
		LOG(LOG_ERR, "open() was with owner=NULL!\n");
		return false;
	}

	if (device == nullptr)
	{
		LOG(LOG_ERR, "open() was with device=NULL!\n");
		return false;
	}

	platformDeviceInfo* dinfo = dynamic_cast<platformDeviceInfo*>(device);
	if (dinfo == NULL)
	{
		LOG(LOG_CRIT, "Could not cast deviceInfo* to platfromDeviceInfo*!\n");
		return false;
	}

	m_owner = owner;
	m_frames = 0;
	m_width = 0;
	m_height = 0;

	HRESULT hr = S_OK;
	ComPtr<IMFActivate> pActivate;
	hr = findCaptureDevice(dinfo->m_devicePath.c_str(), pActivate);
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_CRIT, "Could not find device %s\n", dinfo->m_uniqueID.c_str());
		return false;
	}


	hr = pActivate->ActivateObject(IID_IMFMediaSource, (void**)&m_pMediaSource);
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_ERR, "ActivateObject failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	ComPtr<IMFAttributes> pAttributes;
	MFCreateAttributes(&pAttributes, 10);

	pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, true);
	pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, false);
	pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, false);
	pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, true);

#ifdef USE_SOURCE_READER_ASYNC_CALLBACK
	m_ReaderCB = new SourceReaderCB();
	m_ReaderCB->m_stream = this;
	pAttributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, m_ReaderCB);
#endif

	hr = MFCreateSourceReaderFromMediaSource(m_pMediaSource, pAttributes.Get(), &m_pSourceReader);
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_ERR, "MFCreateSourceReaderFromMediaSource failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	ComPtr<IMFMediaType> pUserSetMediaType;
	MFCreateMediaType(&pUserSetMediaType);

	MFSetAttributeSize(pUserSetMediaType.Get(), MF_MT_FRAME_SIZE, width, height);
	MFSetAttributeSize(pUserSetMediaType.Get(), MF_MT_FRAME_SIZE, width, height);

	UINT32 frameRateNum = (UINT32)round(fps * 1000.0);
	UINT32 frameRateDenom = 1000;
	MFSetAttributeRatio(pUserSetMediaType.Get(), MF_MT_FRAME_RATE, frameRateNum, frameRateDenom);
	pUserSetMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	GUID subType = MFVideoFormat_Base;
	subType.Data1 = fourCC;
	pUserSetMediaType->SetGUID(MF_MT_SUBTYPE, subType);

	hr = m_pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, pUserSetMediaType.Get());
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_ERR, "SetCurrentMediaType failed (HRESULT = %08X)!\n", hr);
		return false;
	}


	ComPtr<IMFMediaType> pCurrentMediaType;
	hr = m_pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentMediaType);
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_ERR, "GetCurrentMediaType failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	{
		MFGetAttributeSize(pCurrentMediaType.Get(), MF_MT_FRAME_SIZE, &m_width, &m_height);
		MFGetAttributeRatio(pCurrentMediaType.Get(), MF_MT_FRAME_RATE, &frameRateNum, &frameRateDenom);
		double framerate = frameRateDenom != 0 ? ((double)frameRateNum) / ((double)frameRateDenom) : 0;
		pCurrentMediaType->GetGUID(MF_MT_SUBTYPE, &subType);

		m_frameBuffer.resize(m_width * m_height * 3);

		LOG(LOG_VERBOSE, "Camera output format %d x %d  %d fps FOURCC=%s\n",
			m_width,
			m_height,
			(int)framerate,
			fourCCToString(subType.Data1).c_str());
	}

	ComPtr<IMFMediaType> pConvertedType;
	MFCreateMediaType(&pConvertedType);
	MFSetAttributeSize(pConvertedType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
	pConvertedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	pConvertedType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB24);

	if (m_transform.IsCompressedMediaType(pCurrentMediaType.Get()))
	{
		ComPtr<IMFMediaType> pUncomprssedType;
		m_transform.InitDecoder(pCurrentMediaType.Get(), pUncomprssedType);
		m_transform.InitColorSpaceTransform(pUncomprssedType.Get(), pConvertedType.Get());
	}
	else
		m_transform.InitColorSpaceTransform(pCurrentMediaType.Get(), pConvertedType.Get());


	m_camControl = nullptr;
	hr = m_pSourceReader->GetServiceForStream((DWORD)MF_SOURCE_READER_MEDIASOURCE, GUID_NULL, IID_PPV_ARGS(&m_camControl));
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_ERR, "Could not create IAMCameraControl\n");
		return false;
	}

	dumpCameraProperties();

	m_videoProcAmp = nullptr;
	hr = m_pSourceReader->GetServiceForStream((DWORD)MF_SOURCE_READER_MEDIASOURCE, GUID_NULL, IID_PPV_ARGS(&m_videoProcAmp));
	if (hr != S_OK)
	{
		LOG(LOG_WARNING, "Could not create IAMVideoProcAmp\n");
	}

#ifdef USE_SOURCE_READER_ASYNC_CALLBACK
	m_bCapture = true;
	if (FAILED(hr = m_pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL)))
	{
		LOG(LOG_ERR, "videoio(MSMF): can't grab frame - initial async ReadSample() call failed: (HRESULT = %08X)!\n", hr);
		m_bCapture = false;
		return false;
	}

	m_isOpen = true;
#else
	m_isOpen = true;
	m_bCapture = true;
	m_readThread = std::thread(&PlatformStreamMF::readThreadFunc, this);
#endif

	return true;
}

void PlatformStreamMF::readThreadFunc()
{
	DWORD dwStreamIndex, dwStreamFlags;
	LONGLONG llTimeStamp;

	HRESULT hr = S_OK;

	m_frames = 0;
	m_newFrame = false;

	while (m_bCapture)
	{
		ComPtr<IMFSample> sample;
		hr = m_pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &dwStreamIndex, &dwStreamFlags, &llTimeStamp, &sample);
		if (FAILED(hr))
		{
			LOG(LOG_ERR, "ReadSample() call failed : (HRESULT = % 08X)!\n", hr);
			break;
		}

		if (dwStreamFlags & MF_SOURCE_READERF_ENDOFSTREAM)
		{
			LOG(LOG_ERR, "ReadSample() end of stream\n");
			break;
		}

		if (dwStreamFlags & MF_SOURCE_READERF_STREAMTICK && !sample)
		{
			continue;
		}

		m_bufferMutex.lock();
		hr = m_transform.DoTransform(sample.Get(), m_frameBuffer);
		m_newFrame = true;
		m_frames++;
		m_bufferMutex.unlock();
		if (FAILED(hr))
		{
			LOG(LOG_ERR, "DoTransform() call failed : (HRESULT = % 08X)!\n", hr);
			break;
		}
	}
}


bool PlatformStreamMF::setFrameRate(uint32_t fps)
{
	//FIXME: implement
	return false;
}

uint32_t PlatformStreamMF::getFOURCC()
{
	if (!m_isOpen) return 0;

	return 0;
}

void PlatformStreamMF::dumpCameraProperties()
{
	LOG(LOG_DEBUG, "------------Camera Properties:------------\n");

	if (m_camControl != 0)
	{
		//query exposure
		long flags, mmin, mmax, delta, defaultValue;
		if (m_camControl->GetRange(CameraControl_Exposure, &mmin, &mmax,
			&delta, &defaultValue, &flags) == S_OK)
		{
			LOG(LOG_DEBUG, "Exposure min     : %2.3f seconds (%d integer)\n", pow(2.0f, (float)mmin), mmin);
			LOG(LOG_DEBUG, "Exposure max     : %2.3f seconds (%d integer)\n", pow(2.0f, (float)mmax), mmax);
			LOG(LOG_DEBUG, "Exposure step    : %d (integer)\n", delta);
			LOG(LOG_DEBUG, "Exposure default : %2.3f seconds\n", pow(2.0f, (float)defaultValue));
			LOG(LOG_DEBUG, "Flags            : %08X\n", flags);
		}
		else
		{
			LOG(LOG_DEBUG, "Could not get exposure range information\n");
		}

		//query focus
		if (m_camControl->GetRange(CameraControl_Focus, &mmin, &mmax,
			&delta, &defaultValue, &flags) == S_OK)
		{
			LOG(LOG_DEBUG, "Focus min     : %d integer\n", mmin);
			LOG(LOG_DEBUG, "Focus max     : %d integer\n", mmax);
			LOG(LOG_DEBUG, "Focus step    : %d integer\n", delta);
			LOG(LOG_DEBUG, "Focus default : %d integer\n", defaultValue);
			LOG(LOG_DEBUG, "Flags         : %08X\n", flags);
		}
		else
		{
			LOG(LOG_DEBUG, "Could not get focus range information\n");
		}

		// query zoom
		if (m_camControl->GetRange(CameraControl_Zoom, &mmin, &mmax,
			&delta, &defaultValue, &flags) == S_OK)
		{
			LOG(LOG_DEBUG, "Zoom min     : %d integer\n", mmin);
			LOG(LOG_DEBUG, "Zoom max     : %d integer\n", mmax);
			LOG(LOG_DEBUG, "Zoom step    : %d integer\n", delta);
			LOG(LOG_DEBUG, "Zoom default : %d integer\n", defaultValue);
			LOG(LOG_DEBUG, "Flags         : %08X\n", flags);
		}
		else
		{
			LOG(LOG_DEBUG, "Could not get Zoom range information\n");
		}

#if 0
		if (m_camControl->get_Exposure(&value, &flags) == S_OK)
		{
			printf("Exposure: %2.3f seconds\n", pow(2.0f, (float)value));
			printf("Flags   : %08X\n", flags);
		}
		else
		{
			printf("Exposure info failed!\n");
		}
#endif       
	}
}

void PlatformStreamMF::OnIncomingCapturedData(IMFSample* sample)
{
	if (sample)
	{
		m_bufferMutex.lock();
		m_transform.DoTransform(sample, m_frameBuffer);
		m_frames++;
		m_newFrame = true;
		m_bufferMutex.unlock();
	}

	if (m_bCapture)
		m_pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL);
}



/** get the limits and default value of a camera/stream property (exposure, zoom etc) */
bool PlatformStreamMF::getPropertyLimits(CapPropertyID propID, int32_t* emin, int32_t* emax, int32_t* dValue)
{
	if ((m_camControl == nullptr) || (emin == nullptr) || (emax == nullptr))
	{
		return false;
	}

	if (propID < CAPPROPID_LAST)
	{
		long flags, mmin, mmax, delta, defaultValue;
		if (gs_properties[propID].isCameraControl)
		{
			// use Camera control
			if (m_camControl->GetRange(gs_properties[propID].dsProp,
				&mmin, &mmax, &delta, &defaultValue, &flags) == S_OK)
			{
				*emin = mmin;
				*emax = mmax;
				*dValue = defaultValue;
				return true;
			}
		}
		else
		{
			// use VideoProcAmp
			if (m_videoProcAmp == nullptr)
			{
				return false; // no VideoProcAmp on board camera
			}

			if (m_videoProcAmp->GetRange(gs_properties[propID].dsProp,
				&mmin, &mmax, &delta, &defaultValue, &flags) == S_OK)
			{
				*emin = mmin;
				*emax = mmax;
				*dValue = defaultValue;
				return true;
			}
		}
	}

	return false;
}


/** set property (exposure, zoom etc) of camera/stream */
bool PlatformStreamMF::setProperty(uint32_t propID, int32_t value)
{
	if (m_camControl == nullptr)
	{
		return false;
	}


	if (propID < CAPPROPID_LAST)
	{
		long flags, dummy;
		if (gs_properties[propID].isCameraControl)
		{
			// use Camera control
			// first we get the property so we can retain the flag settings
			if (m_camControl->Get(gs_properties[propID].dsProp, &dummy, &flags) != S_OK)
			{
				return false;
			}

			// now we set the property.
			if (m_camControl->Set(gs_properties[propID].dsProp, value, flags) != S_OK)
			{
				return false;
			}

			return true;
		}
		else
		{
			// use VideoProcAmp
			if (m_videoProcAmp == nullptr)
			{
				return false; // no VideoProcAmp on board camera
			}

			// first we get the property so we can retain the flag settings
			if (m_videoProcAmp->Get(gs_properties[propID].dsProp, &dummy, &flags) != S_OK)
			{
				return false;
			}

			// now we set the property.
			if (m_videoProcAmp->Set(gs_properties[propID].dsProp, value, flags) != S_OK)
			{
				return false;
			}

			return true;
		}
	}

	return false;
}


/** set automatic state of property (exposure, zoom etc) of camera/stream */
bool PlatformStreamMF::setAutoProperty(uint32_t propID, bool enabled)
{
	if (m_camControl == 0)
	{
		return false;
	}

	long prop = 0;
	switch (propID)
	{
	case CAPPROPID_EXPOSURE:
		prop = CameraControl_Exposure;
		break;
	case CAPPROPID_FOCUS:
		prop = CameraControl_Focus;
		break;
	case CAPPROPID_ZOOM:
		prop = CameraControl_Zoom;
		break;
	case CAPPROPID_WHITEBALANCE:
		prop = VideoProcAmp_WhiteBalance;
		break;
	case CAPPROPID_GAIN:
		prop = VideoProcAmp_Gain;
		break;
	default:
		return false;
	}

	if ((propID != CAPPROPID_WHITEBALANCE) && (propID != CAPPROPID_GAIN))
	{
		//FIXME: check return codes.
		if (enabled)
			m_camControl->Set(prop, 0, CameraControl_Flags_Auto /*| KSPROPERTY_CAMERACONTROL_FLAGS_RELATIVE*/);
		else
			m_camControl->Set(prop, 0, CameraControl_Flags_Manual /*| KSPROPERTY_CAMERACONTROL_FLAGS_RELATIVE*/);
	}
	else
	{
		//note: m_videoProcAmp only exists if the camera
		//      supports hardware accelleration of 
		//      video frame processing, such as
		//      white balance etc.
		if (m_videoProcAmp == nullptr)
		{
			return false;
		}

		// get the current value so we can just set the auto flag
		// but leave the actualy setting itself intact.
		long currentValue, flags;
		if (FAILED(m_videoProcAmp->Get(prop, &currentValue, &flags)))
		{
			return false;
		}

		//FIXME: check return codes.
		if (enabled)
			m_videoProcAmp->Set(prop, currentValue, VideoProcAmp_Flags_Auto);
		else
			m_videoProcAmp->Set(prop, currentValue, VideoProcAmp_Flags_Manual);
	}

	return true;
}

bool PlatformStreamMF::getDSProperty(uint32_t propID, long& value, long& flags)
{
	if (m_camControl == 0)
	{
		return false;
	}

	if (propID < CAPPROPID_LAST)
	{
		if (gs_properties[propID].isCameraControl)
		{
			// use Camera control 
			if (FAILED(m_camControl->Get(gs_properties[propID].dsProp, &value, &flags)))
			{
				return false;
			}
			return true;
		}
		else
		{
			//note: m_videoProcAmp only exists if the camera
			//      supports hardware accelleration of 
			//      video frame processing, such as
			//      white balance etc.
			if (m_videoProcAmp == nullptr)
			{
				return false;
			}

			// get the current value so we can just set the auto flag
			// but leave the actualy setting itself intact.
			if (FAILED(m_videoProcAmp->Get(gs_properties[propID].dsProp, &value, &flags)))
			{
				return false;
			}
			return true;
		}
	}
	return false;
}

/** get property (exposure, zoom etc) of camera/stream */
bool PlatformStreamMF::getProperty(uint32_t propID, int32_t& outValue)
{
	// in keeping with the documentation, we assume long here.. 
	// the DS documentation does not specify the actual bit-width
	// for the vars, but we use 32-bit ints in the capture lib
	// so we convert to 32-bits and hope for the best.. 

	long value, flags;
	if (PlatformStreamMF::getDSProperty(propID, value, flags))
	{
		outValue = value;
		return true;
	}
	return false;
}

/** get automatic state of property (exposure, zoom etc) of camera/stream */
bool PlatformStreamMF::getAutoProperty(uint32_t propID, bool& enabled)
{
	// Here, we assume that 
	// CameraControl_Flags_Auto == VideoProcAmp_Flags_Auto
	// and
	// CameraControl_Flags_Manual == VideoProcAmp_Flags_Manual
	// to simplify the code.
	// We make sure this assumption is true via a static assert

	static_assert(CameraControl_Flags_Auto == VideoProcAmp_Flags_Auto, "Boolean flags dont match - code change needed!\n");
	//static_assert(CameraControl_Flags_Manual == VideoProcAmp_Flags_Manual, "Boolean flags dont match - code change needed!");

	//LOG(LOG_VERBOSE, "PlatformStream::getAutoProperty called\n");

	long value, flags;
	if (PlatformStreamMF::getDSProperty(propID, value, flags))
	{
		enabled = ((flags & CameraControl_Flags_Auto) != 0);
		return true;
	}
	return false;
}


void PlatformStreamMF::submitBuffer(const uint8_t* ptr, size_t bytes)
{
	m_bufferMutex.lock();

	if (m_frameBuffer.size() == 0)
	{
		LOG(LOG_ERR, "Stream::m_frameBuffer size is 0 - cant store frame buffers!\n");
	}

	// Generate warning every 100 frames if the frame buffer is not
	// the expected size. 

	const uint32_t wantSize = m_width * m_height * 3;
	if ((bytes != wantSize) && ((m_frames % 100) == 0))
	{
		LOG(LOG_WARNING, "Warning: captureFrame received incorrect buffer size (got %d want %d)\n", bytes, wantSize);
	}

	if (bytes <= m_frameBuffer.size())
	{
		// The Win32 API delivers upside-down BGR frames.
		// Conversion to regular RGB frames is done by
		// byte-reversing the buffer

		for (size_t y = 0; y < m_height; y++)
		{
			uint8_t* dst = &m_frameBuffer[(y * m_width) * 3];
			const uint8_t* src = ptr + (m_width * 3) * (m_height - y - 1);
			for (uint32_t x = 0; x < m_width; x++)
			{
				uint8_t b = *src++;
				uint8_t g = *src++;
				uint8_t r = *src++;
				*dst++ = r;
				*dst++ = g;
				*dst++ = b;
			}
		}

		m_newFrame = true;
		m_frames++;
	}

	m_bufferMutex.unlock();
}



HRESULT __stdcall SourceReaderCB::QueryInterface(REFIID iid, void** ppv)
{
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4838)
#endif
	static const QITAB qit[] =
	{
		QITABENT(SourceReaderCB, IMFSourceReaderCallback),
		{ 0 },
	};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
	return QISearch(this, qit, iid, ppv);
}

ULONG __stdcall SourceReaderCB::AddRef()
{
	return InterlockedIncrement(&m_nRefCount);
}

ULONG __stdcall SourceReaderCB::Release()
{
	ULONG uCount = InterlockedDecrement(&m_nRefCount);
	if (uCount == 0)
	{
		delete this;
	}
	return uCount;
}

HRESULT __stdcall SourceReaderCB::OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex, DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample)
{
	//CV_UNUSED(llTimestamp);

	HRESULT hr = 0;

	if (SUCCEEDED(hrStatus))
	{
		m_stream->OnIncomingCapturedData(pSample);
	}
	else
	{
		LOG(LOG_WARNING, "SourceReaderCB: OnReadSample() is called with error status: \n");
	}

	if (MF_SOURCE_READERF_ENDOFSTREAM & dwStreamFlags)
	{
		// Reached the end of the stream.
		//m_bCapture = false;
	}

	return S_OK;
}

HRESULT __stdcall SourceReaderCB::OnFlush(DWORD stream_index)
{
	if (m_hEvent != INVALID_HANDLE_VALUE) {
		SetEvent(m_hEvent);
		m_hEvent = INVALID_HANDLE_VALUE;
	}

	return S_OK;
}



MFTColorSpaceTransform::MFTColorSpaceTransform()
{
}

MFTColorSpaceTransform::~MFTColorSpaceTransform()
{
}

static std::string wcharPtrToString(const wchar_t* sstr)
{
	std::vector<char> buffer;
	int32_t chars = WideCharToMultiByte(CP_UTF8, 0, sstr, -1, nullptr, 0, nullptr, nullptr);
	if (chars == 0) return std::string("");

	buffer.resize(chars);
	WideCharToMultiByte(CP_UTF8, 0, sstr, -1, &buffer[0], chars, nullptr, nullptr);
	return std::string(&buffer[0]);
}

bool MFTColorSpaceTransform::InitColorSpaceTransform(IMFMediaType* pInputType, IMFMediaType* pOutputType)
{
	IMFActivate** ppActivates = nullptr;
	UINT32 numActivate = 0; // will be 1
	HRESULT hr = S_OK;

	GUID inputMajorType, inputSubType;
	GUID outputMajorType, outSubType;
	pInputType->GetGUID(MF_MT_MAJOR_TYPE, &inputMajorType);
	pInputType->GetGUID(MF_MT_SUBTYPE, &inputSubType);

	pOutputType->GetGUID(MF_MT_MAJOR_TYPE, &outputMajorType);
	pOutputType->GetGUID(MF_MT_SUBTYPE, &outSubType);

	MFT_REGISTER_TYPE_INFO inputTypeInfo = { inputMajorType, inputSubType };
	MFT_REGISTER_TYPE_INFO outputTypeInfo = { outputMajorType, outSubType };

	hr = MFTEnumEx(MFT_CATEGORY_VIDEO_PROCESSOR,
		MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
		&inputTypeInfo,
		&outputTypeInfo,
		&ppActivates,
		&numActivate);

	if (FAILED(hr) || numActivate <= 0)
	{
		LOG(LOG_ERR, "MFTEnumEx(MFT_CATEGORY_VIDEO_PROCESSOR) failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	LOG(LOG_DEBUG, "# Category 'MFT_CATEGORY_VIDEO_PROCESSOR':\n");
	for (UINT32 i = 0; i < numActivate; ++i)
	{
		WCHAR* ppszName = nullptr;

		UINT32 cchLengh = 0;
		hr = ppActivates[i]->GetAllocatedString(
			MFT_FRIENDLY_NAME_Attribute,
			&ppszName,
			&cchLengh
		);

		LOG(LOG_DEBUG, " * %s\n", wcharPtrToString(ppszName).c_str());

		CoTaskMemFree(ppszName);
	}

	hr = ppActivates[0]->ActivateObject(__uuidof(IMFTransform), (void**)&m_pMFTProcessor);

	for (UINT32 i = 0; i < numActivate; ++i)
		ppActivates[i]->Release();
	CoTaskMemFree(ppActivates);

	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTActivate ActivateObject failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	ComPtr<IMFAttributes> pAttributes;
	m_pMFTProcessor->GetAttributes(&pAttributes);

	UINT32 attrValue = 0;
	if (SUCCEEDED(pAttributes->GetUINT32(MF_SA_D3D11_AWARE, &attrValue)))
	{
		LOG(LOG_DEBUG, "GPU-accelerated video processing supported\n");
	}

	hr = m_pMFTProcessor->SetInputType(0, pInputType, 0);
	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTTransform SetInputType failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	DWORD dwTypeIndex = 0;
	LOG(LOG_DEBUG, "# Colorspace Transform support output format\n");
	while (SUCCEEDED(hr))
	{
		ComPtr<IMFMediaType> pAvailableType;
		hr = m_pMFTProcessor->GetOutputAvailableType(0, dwTypeIndex++, &pAvailableType);
		MediaType type(pAvailableType.Get());

		if (SUCCEEDED(hr))
		{
			GUID guidType;
			pAvailableType->GetGUID(MF_MT_SUBTYPE, &guidType);
			LOG(LOG_DEBUG, "   %s\n", fourCCToStringWin(guidType.Data1).c_str());
		}
	}

	hr = m_pMFTProcessor->SetOutputType(0, pOutputType, 0);
	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTTransform SetOutputType failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	return true;
}


bool MFTColorSpaceTransform::InitDecoder(IMFMediaType* pInputType, ComPtr<IMFMediaType>& pOutputType)
{
	IMFActivate** ppActivates = nullptr;
	UINT32 numActivate = 0; // will be 1
	HRESULT hr = S_OK;

	GUID inputMajorType, inputSubType;
	pInputType->GetGUID(MF_MT_MAJOR_TYPE, &inputMajorType);
	pInputType->GetGUID(MF_MT_SUBTYPE, &inputSubType);

	MFT_REGISTER_TYPE_INFO inputTypeInfo = { inputMajorType, inputSubType };

	hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
		MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
		&inputTypeInfo,
		NULL,
		&ppActivates,
		&numActivate);

	if (FAILED(hr) || numActivate <= 0)
	{
		LOG(LOG_ERR, "MFTEnumEx failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	LOG(LOG_DEBUG, "# Category 'MFT_CATEGORY_VIDEO_DECODER':\n");
	for (UINT32 i = 0; i < numActivate; ++i)
	{
		WCHAR* ppszName = nullptr;

		UINT32 cchLengh = 0;
		hr = ppActivates[i]->GetAllocatedString(
			MFT_FRIENDLY_NAME_Attribute,
			&ppszName,
			&cchLengh
		);

		LOG(LOG_ERR, " * %s\n", wcharPtrToString(ppszName).c_str());

		CoTaskMemFree(ppszName);
	}

	hr = ppActivates[0]->ActivateObject(__uuidof(IMFTransform), (void**)&m_pMFTDecoder);

	for (UINT32 i = 0; i < numActivate; ++i)
		ppActivates[i]->Release();
	CoTaskMemFree(ppActivates);

	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTActivate ActivateObject failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	ComPtr<IMFAttributes> pAttributes;
	m_pMFTDecoder->GetAttributes(&pAttributes);

	UINT32 attrValue = 0;
	if (SUCCEEDED(pAttributes->GetUINT32(MF_SA_D3D11_AWARE, &attrValue)))
	{
		LOG(LOG_DEBUG, "GPU-accelerated video decoding supported\n");
	}

	hr = m_pMFTDecoder->SetInputType(0, pInputType, 0);
	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTTransform SetInputType failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	DWORD dwTypeIndex = 0;
	LOG(LOG_DEBUG, "# Decoder support output format\n");
	while (SUCCEEDED(hr))
	{
		ComPtr<IMFMediaType> pAvailableType;
		hr = m_pMFTDecoder->GetOutputAvailableType(0, dwTypeIndex++, &pAvailableType);
		MediaType type(pAvailableType.Get());

		if (SUCCEEDED(hr))
		{
			GUID guidType;
			pAvailableType->GetGUID(MF_MT_SUBTYPE, &guidType);
			LOG(LOG_DEBUG, "   %s\n", fourCCToStringWin(guidType.Data1).c_str());
		}
	}

	if (dwTypeIndex > 1)
		hr = m_pMFTDecoder->GetOutputAvailableType(0, 0, &pOutputType);
	else
		return false;


	hr = m_pMFTDecoder->SetOutputType(0, pOutputType.Get(), 0);
	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTTransform SetOutputType failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	return true;
}

bool MFTColorSpaceTransform::IsCompressedMediaType(IMFMediaType* inputType)
{
	GUID majorType, subType;
	inputType->GetGUID(MF_MT_MAJOR_TYPE, &majorType);
	inputType->GetGUID(MF_MT_SUBTYPE, &subType);
	GUID compressedTypes[] = {
		MFVideoFormat_MP43,
		MFVideoFormat_MP4S,
		MFVideoFormat_M4S2,
		MFVideoFormat_MP4V,
		MFVideoFormat_WMV1,
		MFVideoFormat_WMV2,
		MFVideoFormat_WMV3,
		MFVideoFormat_WVC1,
		MFVideoFormat_MSS1,
		MFVideoFormat_MSS2,
		MFVideoFormat_MPG1,
		MFVideoFormat_DVSL,
		MFVideoFormat_DVSD,
		MFVideoFormat_DVHD,
		MFVideoFormat_DV25,
		MFVideoFormat_DV50,
		MFVideoFormat_DVH1,
		MFVideoFormat_DVC,
		MFVideoFormat_H264,
		MFVideoFormat_H265,
		MFVideoFormat_MJPG,
		MFVideoFormat_420O,
		MFVideoFormat_HEVC,
		MFVideoFormat_HEVC_ES,
		MFVideoFormat_VP80,
		MFVideoFormat_VP90,
		MFVideoFormat_H263,
		MFVideoFormat_VP10,
		MFVideoFormat_AV1,
	};

	for (int i = 0; i < sizeof(compressedTypes) / sizeof(GUID); ++i)
	{
		if (subType == compressedTypes[i])
			return true;
	}

	return false;
}

HRESULT MFTColorSpaceTransform::DoTransform(IMFSample* pSample, std::vector<BYTE>& outBuffer)
{
	HRESULT hr = S_OK;
	DWORD status = 0;

	ComPtr<IMFSample> pUnconprseedSample;
	ComPtr<IMFMediaBuffer> pUnconprseedBuffer;
	if (m_pMFTDecoder)
	{
		ComPtr<IMFMediaType> outputType;
		hr = m_pMFTDecoder->GetOutputCurrentType(0, &outputType); if (FAILED(hr)) return hr;
		hr = MFCreateMediaBufferFromMediaType(outputType.Get(), 0, 0, 0, &pUnconprseedBuffer); if (FAILED(hr)) return hr;
		hr = MFCreateSample(&pUnconprseedSample); if (FAILED(hr)) return hr;
		hr = pUnconprseedSample->AddBuffer(pUnconprseedBuffer.Get()); if (FAILED(hr)) return hr;

		MFT_OUTPUT_DATA_BUFFER data_buffer = { 0, pUnconprseedSample.Get(), 0, nullptr };

		do
		{
			hr = m_pMFTDecoder->ProcessInput(0, pSample, 0);
			if (FAILED(hr))
				return hr;

			hr = m_pMFTDecoder->ProcessOutput(0, 1, &data_buffer, &status);
			if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT)
				return hr;

		} while (hr == MF_E_TRANSFORM_NEED_MORE_INPUT);

		pSample = pUnconprseedSample.Get();
	}


	ComPtr<IMFMediaType> outputType;
	ComPtr<IMFMediaBuffer> mediaBuffer;
	ComPtr<IMFSample> outSample;
	ComPtr<IMF2DBuffer> buf2d;

	hr = m_pMFTProcessor->GetOutputCurrentType(0, &outputType); if (FAILED(hr)) return hr;
	hr = MFCreateMediaBufferFromMediaType(outputType.Get(), 0, 0, 0, &mediaBuffer); if (FAILED(hr)) return hr;
	hr = MFCreateSample(&outSample); if (FAILED(hr)) return hr;
	hr = outSample->AddBuffer(mediaBuffer.Get()); if (FAILED(hr)) return hr;

	MFT_OUTPUT_DATA_BUFFER rgb24Buffer = { 0, outSample.Get(), 0, nullptr };

	hr = m_pMFTProcessor->ProcessInput(0, pSample, 0); if (FAILED(hr)) return hr;
	hr = m_pMFTProcessor->GetOutputStatus(&status); if (FAILED(hr)) return hr;

	if (status & MFT_OUTPUT_STATUS_SAMPLE_READY) {

		hr = m_pMFTProcessor->ProcessOutput(0, 1, &rgb24Buffer, &status); if (FAILED(hr)) return hr;

		hr = mediaBuffer->QueryInterface(__uuidof(IMF2DBuffer), (void**)&buf2d);

		if (SUCCEEDED(hr))
		{
			DWORD cbLengh = 0;
			buf2d->GetContiguousLength(&cbLengh);
			outBuffer.resize(cbLengh);
			buf2d->ContiguousCopyTo(outBuffer.data(), (DWORD)outBuffer.size());
			buf2d->Release();
		}
	}

	return hr;
}
