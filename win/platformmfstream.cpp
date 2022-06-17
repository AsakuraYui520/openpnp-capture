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
#include "platformmfstream.h"
#include "platformmfcontext.h"
#include <cmath>


#ifdef min
#undef min
#undef max
#endif
enum { MEDIA_TYPE_INDEX_DEFAULT = 0xffffffff };

/** convert a FOURCC uint32_t to human readable form */
static std::string fourCCToStringMF(uint32_t fourcc)
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

static inline  bool qFuzzyCompare(double p1, double p2)
{
	return (abs(p1 - p2) * 1000000000000. <= std::min(abs(p1), abs(p2)));
}

static inline  bool qFuzzyCompare(float p1, float p2)
{
	return (abs(p1 - p2) * 100000.f <= std::min(abs(p1), abs(p2)));
}

extern std::wstring getIMFAttributesString(IMFAttributes* pAttributes, REFGUID guidKey);
extern std::string wstringToString(const std::wstring& wstr);
extern std::string wcharPtrToString(const wchar_t* sstr);

Stream* createPlatformStream()
{
	return new PlatformMFStream();
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

PlatformMFStream::PlatformMFStream() :
	Stream(),
	m_videoSource(nullptr),
	m_sourceReader(nullptr),
	m_camControl(nullptr),
	m_videoProcAmp(nullptr),
	m_videoMediaType(nullptr),
	m_streaming(false)
{

}

PlatformMFStream::~PlatformMFStream()
{
	close();
}

void PlatformMFStream::close()
{
	stopStreaming();

	m_owner = nullptr;
	m_width = 0;
	m_height = 0;
	m_frameBuffer.resize(0);
	m_isOpen = false;
}


bool PlatformMFStream::open(Context* owner, deviceInfo* device, uint32_t width, uint32_t height,
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

	ComPtr<IMFAttributes> sourceAttributes;
	hr = MFCreateAttributes(sourceAttributes.GetAddressOf(), 2);
	hr = sourceAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	hr = sourceAttributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, dinfo->m_devicePath.c_str());

	hr = MFCreateDeviceSource(sourceAttributes.Get(), &m_videoSource);

	if (!SUCCEEDED(hr))
	{
		LOG(LOG_CRIT, "Could not find device %s\n", dinfo->m_uniqueID.c_str());
		return false;
	}

	ComPtr<IMFAttributes> readerAttributes;
	hr = MFCreateAttributes(readerAttributes.GetAddressOf(), 1);
	readerAttributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, this);


	hr = MFCreateSourceReaderFromMediaSource(m_videoSource, readerAttributes.Get(), &m_sourceReader);
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_ERR, "MFCreateSourceReaderFromMediaSource failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	DWORD mediaTypeIndex = findMediaTypeIndex(width, height, fourCC, fps);

	if (!SUCCEEDED(prepareVideoStream(mediaTypeIndex))) {
		LOG(LOG_ERR, "prepareVideoStream failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	m_camControl = nullptr;
	hr = m_sourceReader->GetServiceForStream((DWORD)MF_SOURCE_READER_MEDIASOURCE, GUID_NULL, IID_PPV_ARGS(&m_camControl));
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_ERR, "Could not create IAMCameraControl\n");
		return false;
	}

	dumpCameraProperties();

	m_videoProcAmp = nullptr;
	hr = m_sourceReader->GetServiceForStream((DWORD)MF_SOURCE_READER_MEDIASOURCE, GUID_NULL, IID_PPV_ARGS(&m_videoProcAmp));
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_WARNING, "Could not create IAMVideoProcAmp\n");
	}

	hr = m_sourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL);
	if (!SUCCEEDED(hr))
	{
		LOG(LOG_ERR, "ReadSample() call failed: (HRESULT = %08X)!\n", hr);
		return false;
	}

	m_isOpen = true;

	return true;
}


bool PlatformMFStream::setFrameRate(uint32_t fps)
{
	std::unique_lock<std::mutex> locker(m_mutex);
	//FIXME: implement
	return false;
}

uint32_t PlatformMFStream::getFOURCC()
{
	if (!m_isOpen) return 0;

	return 0;
}

void PlatformMFStream::dumpCameraProperties()
{
	LOG(LOG_DEBUG, "------------Camera Properties:------------\n");

	if (m_camControl != nullptr)
	{
		//query exposure
		long flags, mmin, mmax, delta, defaultValue;
		if (m_camControl->GetRange(CameraControl_Exposure, &mmin, &mmax,
			&delta, &defaultValue, &flags) == S_OK)
		{
			LOG(LOG_DEBUG, "Exposure min     : %2.3f seconds (%d integer)\n", std::pow(2.0f, (float)mmin), mmin);
			LOG(LOG_DEBUG, "Exposure max     : %2.3f seconds (%d integer)\n", std::pow(2.0f, (float)mmax), mmax);
			LOG(LOG_DEBUG, "Exposure step    : %d (integer)\n", delta);
			LOG(LOG_DEBUG, "Exposure default : %2.3f seconds\n", std::pow(2.0f, (float)defaultValue));
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



/** get the limits and default value of a camera/stream property (exposure, zoom etc) */
bool PlatformMFStream::getPropertyLimits(CapPropertyID propID, int32_t* emin, int32_t* emax, int32_t* dValue)
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
bool PlatformMFStream::setProperty(uint32_t propID, int32_t value)
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
bool PlatformMFStream::setAutoProperty(uint32_t propID, bool enabled)
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

bool PlatformMFStream::getDSProperty(uint32_t propID, long& value, long& flags)
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

void PlatformMFStream::stopStreaming()
{
	std::unique_lock<std::mutex> locker(m_mutex);

	LOG(LOG_INFO, "stop streaming\n");

	if (m_videoMediaType) {
		m_videoMediaType->Release();
		m_videoMediaType = nullptr;
	}

	if (m_camControl) {
		m_camControl->Release();
		m_camControl = nullptr;
	}

	if (m_videoProcAmp) {
		m_videoProcAmp->Release();
		m_videoProcAmp = nullptr;
	}

	if (m_sourceReader) {
		m_sourceReader->Release();
		m_sourceReader = nullptr;
	}

	if (m_videoSource) {
		m_videoSource->Shutdown();
		m_videoSource->Release();
		m_videoSource = nullptr;
	}

	m_streaming = false;
}

/** get property (exposure, zoom etc) of camera/stream */
bool PlatformMFStream::getProperty(uint32_t propID, int32_t& outValue)
{
	// in keeping with the documentation, we assume long here.. 
	// the DS documentation does not specify the actual bit-width
	// for the vars, but we use 32-bit ints in the capture lib
	// so we convert to 32-bits and hope for the best.. 

	long value, flags;
	if (PlatformMFStream::getDSProperty(propID, value, flags))
	{
		outValue = value;
		return true;
	}
	return false;
}

/** get automatic state of property (exposure, zoom etc) of camera/stream */
bool PlatformMFStream::getAutoProperty(uint32_t propID, bool& enabled)
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
	if (PlatformMFStream::getDSProperty(propID, value, flags))
	{
		enabled = ((flags & CameraControl_Flags_Auto) != 0);
		return true;
	}
	return false;
}

//from IUnknown
STDMETHODIMP PlatformMFStream::QueryInterface(REFIID riid, LPVOID* ppvObject)
{
	if (!ppvObject)
		return E_POINTER;
	if (riid == IID_IMFSourceReaderCallback) {
		*ppvObject = static_cast<IMFSourceReaderCallback*>(this);
	}
	//else if (riid == IID_IMFSinkWriterCallback) {
	//	*ppvObject = static_cast<IMFSinkWriterCallback*>(this);
	//}
	else if (riid == IID_IUnknown) {
		*ppvObject = static_cast<IUnknown*>(static_cast<IMFSourceReaderCallback*>(this));
	}
	else {
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}
	AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) PlatformMFStream::AddRef(void)
{
	return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG) PlatformMFStream::Release(void)
{
	LONG cRef = InterlockedDecrement(&m_cRef);
	if (cRef == 0) {
		delete this;
	}
	return cRef;
}


//from IMFSourceReaderCallback
STDMETHODIMP PlatformMFStream::OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex,
	DWORD dwStreamFlags, LONGLONG llTimestamp,
	IMFSample* pSample)
{
	std::unique_lock<std::mutex> locker(m_mutex);

	if (FAILED(hrStatus)) {
		//emit streamingError(int(hrStatus));
		return hrStatus;
	}

	if (dwStreamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
		m_streaming = false;
		//emit streamingStopped();
	}
	else {

		if (!m_streaming) {
			m_streaming = true;
			//emit streamingStarted();
		}
		if (pSample)
		{
			m_bufferMutex.lock();
			m_transform.DoTransform(pSample, m_frameBuffer);
			m_frames++;
			m_newFrame = true;
			m_bufferMutex.unlock();
		}

		if (m_sourceReader)
			m_sourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL, NULL);
	}

	return S_OK;
}

STDMETHODIMP PlatformMFStream::OnFlush(DWORD)
{
	return S_OK;
}

STDMETHODIMP PlatformMFStream::OnEvent(DWORD, IMFMediaEvent*)
{
	return S_OK;
}

HRESULT PlatformMFStream::prepareVideoStream(DWORD mediaTypeIndex)
{
	if (!m_sourceReader || !m_videoSource)
		return E_FAIL;

	HRESULT hr = S_OK;

	if (mediaTypeIndex == MEDIA_TYPE_INDEX_DEFAULT) {
		hr = m_sourceReader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
			&m_videoMediaType);
	}
	else {
		hr = m_sourceReader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
			mediaTypeIndex, &m_videoMediaType);
		if (SUCCEEDED(hr))
			hr = m_sourceReader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
				nullptr, m_videoMediaType);
	}

	if (SUCCEEDED(hr)) {

		hr = MFGetAttributeSize(m_videoMediaType, MF_MT_FRAME_SIZE, &m_width, &m_height);

		if (SUCCEEDED(hr)) {

			ComPtr<IMFMediaType> convertedType;
			hr = MFCreateMediaType(convertedType.GetAddressOf());

			if (SUCCEEDED(hr)) {

				MFSetAttributeSize(convertedType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
				convertedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
				convertedType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB24);

				if (m_transform.IsCompressedMediaType(m_videoMediaType))
				{
					ComPtr<IMFMediaType> decodedType;
					if (m_transform.InitVideoDecoder(m_videoMediaType, decodedType.GetAddressOf())) {
						m_transform.InitVideoProcessor(decodedType.Get(), convertedType.Get());
					}
				}
				else
					m_transform.InitVideoProcessor(m_videoMediaType, convertedType.Get());
			}
		}
	}

	return hr;

}

DWORD PlatformMFStream::findMediaTypeIndex(int32_t reqWidth, uint32_t reqHeight, uint32_t reqFourCC, uint32_t reqFrameRate)
{
	DWORD mediaIndex = MEDIA_TYPE_INDEX_DEFAULT;

	if (m_sourceReader && m_videoSource) {

		DWORD index = 0;
		IMFMediaType* mediaType = nullptr;

		UINT32 currArea = 0;
		float currFrameRate = 0.0f;

		while (SUCCEEDED(m_sourceReader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
			index, &mediaType))) {

			GUID subtype = GUID_NULL;
			if (SUCCEEDED(mediaType->GetGUID(MF_MT_SUBTYPE, &subtype))) {

				UINT32 width, height;
				if (SUCCEEDED(MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &width, &height))) {

					UINT32 num, den;
					if (SUCCEEDED(MFGetAttributeRatio(mediaType, MF_MT_FRAME_RATE, &num, &den))) {

						UINT32 area = width * height;
						float frameRate = float(num) / den;

						if (reqWidth == width
							&& reqHeight == height
							&& qFuzzyCompare((float)reqFrameRate, frameRate)
							&& reqFourCC == subtype.Data1) {
							mediaType->Release();
							return index;
						}

						if ((currFrameRate < 29.9 && currFrameRate < frameRate) ||
							(currFrameRate == frameRate && currArea < area)) {
							currArea = area;
							currFrameRate = frameRate;
							mediaIndex = index;
						}
					}
				}

			}
			mediaType->Release();
			++index;
		}
	}

	return mediaIndex;
}

MFTColorSpaceTransform::MFTColorSpaceTransform() :
	m_videoProcessor(nullptr),
	m_videoDecoder(nullptr)
{
}

MFTColorSpaceTransform::~MFTColorSpaceTransform()
{
	if (m_videoDecoder) {
		m_videoDecoder->Release();
		m_videoDecoder = nullptr;
	}

	if (m_videoProcessor) {
		m_videoProcessor->Release();
		m_videoProcessor = nullptr;
	}
}



bool MFTColorSpaceTransform::InitVideoProcessor(IMFMediaType* pInputType, IMFMediaType* pOutputType)
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
		std::wstring name = getIMFAttributesString(ppActivates[i], MFT_FRIENDLY_NAME_Attribute);

		LOG(LOG_DEBUG, " * %s\n", wstringToString(name).c_str());
	}

	hr = ppActivates[0]->ActivateObject(IID_PPV_ARGS(&m_videoProcessor));

	for (UINT32 i = 0; i < numActivate; ++i)
		ppActivates[i]->Release();
	CoTaskMemFree(ppActivates);

	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTActivate ActivateObject failed (HRESULT = %08X)!\n", hr);
		return false;
	}


	hr = m_videoProcessor->SetInputType(0, pInputType, 0);
	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTTransform SetInputType failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	DWORD dwTypeIndex = 0;
	LOG(LOG_DEBUG, "# Colorspace Transform support output format\n");
	while (SUCCEEDED(hr))
	{
		ComPtr<IMFMediaType> availableType;
		hr = m_videoProcessor->GetOutputAvailableType(0, dwTypeIndex++, availableType.GetAddressOf());

		if (SUCCEEDED(hr) && availableType)
		{
			GUID guidType;
			availableType->GetGUID(MF_MT_SUBTYPE, &guidType);
			LOG(LOG_DEBUG, "   %s\n", fourCCToStringMF(guidType.Data1).c_str());
		}
	}

	hr = m_videoProcessor->SetOutputType(0, pOutputType, 0);
	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTTransform SetOutputType failed (HRESULT = %08X)!\n", hr);
		return false;
	}


	MFGetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, &m_width, &m_height);

	/*hr = m_videoProcessor->GetOutputCurrentType(0, outputType.GetAddressOf());
	if (FAILED(hr))
		return false;

	hr = MFCreateMediaBufferFromMediaType(outputType.Get(), 0, 0, 0, outMediaBuffer.GetAddressOf());
	if (FAILED(hr))
		return false;

	hr = MFCreateSample(outSample.GetAddressOf());
	if (FAILED(hr))
		return false;

	hr = outSample->AddBuffer(outMediaBuffer.Get());
	if (FAILED(hr))
		return false;*/

	return true;
}


bool MFTColorSpaceTransform::InitVideoDecoder(IMFMediaType* pInputType, IMFMediaType** ppOutputType)
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

	if (FAILED(hr) || numActivate == 0)
	{
		LOG(LOG_ERR, "MFTEnumEx failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	LOG(LOG_DEBUG, "# Category 'MFT_CATEGORY_VIDEO_DECODER':\n");
	for (UINT32 i = 0; i < numActivate; ++i)
	{
		std::wstring name = getIMFAttributesString(ppActivates[i], MFT_FRIENDLY_NAME_Attribute);

		LOG(LOG_DEBUG, " * %s\n", wstringToString(name).c_str());
	}

	hr = ppActivates[0]->ActivateObject(IID_PPV_ARGS(&m_videoDecoder));

	for (UINT32 i = 0; i < numActivate; ++i)
		ppActivates[i]->Release();
	CoTaskMemFree(ppActivates);

	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTActivate ActivateObject failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	ComPtr<IMFAttributes> decoderAttributes;
	m_videoDecoder->GetAttributes(decoderAttributes.GetAddressOf());

	UINT32 attrValue = 0;
	if (SUCCEEDED(decoderAttributes->GetUINT32(MF_SA_D3D11_AWARE, &attrValue)))
	{
		LOG(LOG_DEBUG, "GPU-accelerated video decoding supported\n");
	}

	hr = m_videoDecoder->SetInputType(0, pInputType, 0);
	if (FAILED(hr))
	{
		LOG(LOG_ERR, "MFTTransform SetInputType failed (HRESULT = %08X)!\n", hr);
		return false;
	}

	DWORD dwTypeIndex = 0;
	LOG(LOG_DEBUG, "# Decoder support output format\n");
	while (SUCCEEDED(hr))
	{
		ComPtr<IMFMediaType> availableType;
		hr = m_videoDecoder->GetOutputAvailableType(0, dwTypeIndex++, availableType.GetAddressOf());

		if (SUCCEEDED(hr) && availableType)
		{
			GUID guidType;
			availableType->GetGUID(MF_MT_SUBTYPE, &guidType);
			LOG(LOG_DEBUG, "   %s\n", fourCCToStringMF(guidType.Data1).c_str());
		}
	}

	if (dwTypeIndex > 1)
		hr = m_videoDecoder->GetOutputAvailableType(0, 0, ppOutputType);
	else
		return false;


	hr = m_videoDecoder->SetOutputType(0, *ppOutputType, 0);
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
	DWORD dwStatus = 0;

	ComPtr<IMFSample> uncompressedSample;

	if (m_videoDecoder)
	{
		MFT_OUTPUT_STREAM_INFO streamInfo = { 0 };
		hr = m_videoDecoder->GetOutputStreamInfo(0, &streamInfo);
		if (FAILED(hr))
			return hr;

		ComPtr<IMFMediaBuffer> uncompressedBuffer;

		hr = MFCreateAlignedMemoryBuffer(streamInfo.cbSize, streamInfo.cbAlignment, uncompressedBuffer.GetAddressOf());
		if (FAILED(hr))
			return hr;

		hr = MFCreateSample(uncompressedSample.GetAddressOf());
		if (FAILED(hr))
			return hr;

		uncompressedSample->AddBuffer(uncompressedBuffer.Get());

		hr = m_videoDecoder->ProcessInput(0, pSample, 0);
		if (FAILED(hr))
			return hr;

		MFT_OUTPUT_DATA_BUFFER output_data_buffer = { 0, uncompressedSample.Get(), 0, nullptr };

		hr = m_videoDecoder->ProcessOutput(0, 1, &output_data_buffer, &dwStatus);

		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			return hr;
		}
		else if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
		{
			//H264 todo
			return hr;
		}
		else if (FAILED(hr))
			return hr;

		pSample = uncompressedSample.Get();
	}


	ComPtr<IMFMediaBuffer> outMediaBuffer;
	ComPtr<IMFSample> outSample;

	MFT_OUTPUT_STREAM_INFO streamInfo = { 0 };
	hr = m_videoProcessor->GetOutputStreamInfo(0, &streamInfo);
	if (FAILED(hr))
		return hr;

	hr = MFCreateAlignedMemoryBuffer(streamInfo.cbSize, streamInfo.cbAlignment, outMediaBuffer.GetAddressOf());
	if (FAILED(hr))
		return hr;

	hr = MFCreateSample(outSample.GetAddressOf());
	if (FAILED(hr))
		return hr;

	hr = outSample->AddBuffer(outMediaBuffer.Get());
	if (FAILED(hr))
		return hr;

	hr = m_videoProcessor->ProcessInput(0, pSample, 0);
	if (FAILED(hr))
		return hr;

	MFT_OUTPUT_DATA_BUFFER outputSamples = { 0, outSample.Get(), 0, nullptr };

	hr = m_videoProcessor->ProcessOutput(0, 1, &outputSamples, &dwStatus);
	if (FAILED(hr))
		return hr;

	BYTE* pbBuffer = nullptr;
	DWORD cbMaxLengh = 0, cbCurrentLenth = 0;

	if (SUCCEEDED(outMediaBuffer->Lock(&pbBuffer, &cbMaxLengh, &cbCurrentLenth)))
	{
		outBuffer.resize(cbCurrentLenth);

		for (UINT32 y = 0; y < m_height; y++)
		{
			uint8_t* dst = &outBuffer[(y * m_width) * 3];
			const uint8_t* src = pbBuffer + (m_width * 3) * (m_height - y - 1);
			for (UINT32 x = 0; x < m_width; x++)
			{
				uint8_t b = *src++;
				uint8_t g = *src++;
				uint8_t r = *src++;
				*dst++ = b;
				*dst++ = g;
				*dst++ = r;
			}
		}
		outMediaBuffer->Unlock();
	}

	return hr;
}


