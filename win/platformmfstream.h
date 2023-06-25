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

#ifndef win_platformstream_mf_h
#define win_platformstream_mf_h


#include <windows.h>
#include <guiddef.h>
#include <mfidl.h>
#include <mfapi.h>
#include <mfplay.h>
#include <mfobjects.h>
#include <tchar.h>
#include <strsafe.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <assert.h>
#include <stdint.h>
#include <vector>
#include <mutex>
#include "../common/logging.h"
#include "../common/stream.h"

template <class T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

class Context;         // pre-declaration
class PlatformStreamMF;  // pre-declaration


class MFTColorSpaceTransform
{
public:
	MFTColorSpaceTransform();
	~MFTColorSpaceTransform();

	bool InitVideoProcessor(IMFMediaType* inputType, IMFMediaType* outputType);
	bool InitVideoDecoder(IMFMediaType* inputType, IMFMediaType** outputType);
	bool IsCompressedMediaType(IMFMediaType* inputType);
	HRESULT DoTransform(IMFSample* input, std::vector<BYTE>& outBuffer);

private:
	IMFTransform* m_videoProcessor;
	IMFTransform* m_videoDecoder;
	UINT32 m_width, m_height;

	//ComPtr<IMFMediaType> outputType;
	//ComPtr<IMFMediaBuffer> outMediaBuffer;
	//ComPtr<IMFSample> outSample;
};


/** The stream class handles the capturing of a single device */
class PlatformMFStream : public Stream, public IMFSourceReaderCallback
{
public:
	PlatformMFStream();
	~PlatformMFStream() override;


	/** Open a capture stream to a device and request a specific (internal) stream format.
		When successfully opened, capturing starts immediately.
	*/
	bool open(Context* owner, deviceInfo* device, uint32_t width, uint32_t height,
		uint32_t fourCC, uint32_t fps) override;

	/** Close a capture stream */
	void close() override;

	/** set the frame rate */
	bool setFrameRate(uint32_t fps) override;

	/** Return the FOURCC media type of the stream */
	uint32_t getFOURCC() override;

	/** get the limits of a camera/stream property (exposure, zoom etc) */
	bool getPropertyLimits(uint32_t propID, int32_t* min, int32_t* max, int32_t* dValue) override;

	/** set property (exposure, zoom etc) of camera/stream */
	bool setProperty(uint32_t propID, int32_t value) override;

	/** set automatic state of property (exposure, zoom etc) of camera/stream */
	bool setAutoProperty(uint32_t propID, bool enabled) override;

	/** get property (exposure, zoom etc) of camera/stream */
	bool getProperty(uint32_t propID, int32_t& outValue) override;

	/** get automatic state of property (exposure, zoom etc) of camera/stream */
	bool getAutoProperty(uint32_t propID, bool& enabled) override;

	//from IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, LPVOID* ppvObject) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;

	//from IMFSourceReaderCallback
	STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex,
		DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample) override;
	STDMETHODIMP OnFlush(DWORD dwStreamIndex) override;
	STDMETHODIMP OnEvent(DWORD dwStreamIndex, IMFMediaEvent* pEvent) override;
protected:
	/** get DirectShow property + flags helper function */
	bool getDSProperty(uint32_t propID, long& value, long& flags);
	void stopStreaming();

	void dumpCameraProperties();

	HRESULT prepareVideoStream(DWORD mediaTypeIndex);

	DWORD findMediaTypeIndex(int32_t width, uint32_t height, uint32_t fourCC, uint32_t fps);

	long m_cRef = 1;
	IMFMediaSource* m_videoSource;
	IMFSourceReader* m_sourceReader;
	IAMCameraControl* m_camControl;
	IAMVideoProcAmp* m_videoProcAmp;
	IMFMediaType* m_videoMediaType;

	MFTColorSpaceTransform m_transform;
	bool m_streaming;
	std::mutex m_mutex;
};



#endif

