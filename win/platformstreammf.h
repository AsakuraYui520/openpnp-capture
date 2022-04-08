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



#include <assert.h>

#include <stdint.h>
#include <vector>
#include <mutex>
#include "../common/logging.h"
#include "../common/stream.h"
#include "scopedcomptr.h"

//#define USE_SOURCE_READER_ASYNC_CALLBACK

class Context;         // pre-declaration
class PlatformStreamMF;  // pre-declaration

/** A class to handle callbacks from the video subsystem,
    A call is made for every frame.

    Note that the callback will be called by the DirectShow thread
    and should return as quickly as possible to avoid interference
    with the capturing process.
*/
class SourceReaderCB : public IMFSourceReaderCallback
{
public:
	SourceReaderCB() :
		m_nRefCount(0),
		m_hEvent(INVALID_HANDLE_VALUE),
		m_stream(NULL)
	{
	}

	// IUnknown methods
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override;
	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;

	//IMFSourceReaderCallback methods
	HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex, DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample) override;

	HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent*) { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnFlush(DWORD);

	void SetSignalOnFlush(HANDLE event) {
		m_hEvent = event;
	}
private:
	// Destructor is private. Caller should call Release.
	virtual ~SourceReaderCB() {}
public:
	long                m_nRefCount;        // Reference count.
	HANDLE              m_hEvent;
	PlatformStreamMF*     m_stream;
};

class MFTColorSpaceTransform
{
public:
	MFTColorSpaceTransform();
	~MFTColorSpaceTransform();

	bool InitColorSpaceTransform(IMFMediaType* inputType, IMFMediaType* outputType);
	bool InitDecoder(IMFMediaType* inputType, ComPtr<IMFMediaType>& outputType);
	bool IsCompressedMediaType(IMFMediaType* inputType);
	HRESULT DoTransform(IMFSample* input,std::vector<BYTE>& outBuffer);

private:
	ComPtr<IMFTransform> m_pMFTProcessor;
	ComPtr<IMFTransform> m_pMFTDecoder;
};


/** The stream class handles the capturing of a single device */
class PlatformStreamMF : public Stream
{
	friend SourceReaderCB;

public:
	PlatformStreamMF();
	virtual ~PlatformStreamMF();


	/** Open a capture stream to a device and request a specific (internal) stream format.
		When succesfully opened, capturing starts immediately.
	*/
	virtual bool open(Context* owner, deviceInfo* device, uint32_t width, uint32_t height,
		uint32_t fourCC, uint32_t fps) override;

	/** Close a capture stream */
	virtual void close() override;

	/** set the frame rate */
	virtual bool setFrameRate(uint32_t fps) override;

	/** Return the FOURCC media type of the stream */
	virtual uint32_t getFOURCC() override;

	/** get the limits of a camera/stream property (exposure, zoom etc) */
	virtual bool getPropertyLimits(uint32_t propID, int32_t* min, int32_t* max, int32_t* dValue) override;

	/** set property (exposure, zoom etc) of camera/stream */
	virtual bool setProperty(uint32_t propID, int32_t value) override;

	/** set automatic state of property (exposure, zoom etc) of camera/stream */
	virtual bool setAutoProperty(uint32_t propID, bool enabled) override;

	/** get property (exposure, zoom etc) of camera/stream */
	virtual bool getProperty(uint32_t propID, int32_t& outValue) override;

	/** get automatic state of property (exposure, zoom etc) of camera/stream */
	virtual bool getAutoProperty(uint32_t propID, bool& enabled) override;

protected:
	/** A re-implementation of Stream::submitBuffer with BGR to RGB conversion */
	virtual void submitBuffer(const uint8_t* ptr, size_t bytes) override;

	/** get DirectShow property + flags helper function */
	bool getDSProperty(uint32_t propID, long& value, long& flags);

	void dumpCameraProperties();

	void OnIncomingCapturedData(IMFSample* sample);

	void readThreadFunc();

	IMFMediaSource* m_pMediaSource;
	IMFSourceReader* m_pSourceReader;
	IAMCameraControl* m_camControl;
	IAMVideoProcAmp* m_videoProcAmp;

	MFTColorSpaceTransform m_transform;
	SourceReaderCB* m_ReaderCB;
	std::thread m_readThread;
	bool m_bCapture;
};


// Structure for collecting info about types of video which are supported by current video device
struct MediaType
{
	UINT32 width;
	UINT32 height;
	INT32 stride; // stride is negative if image is bottom-up
	UINT32 isFixedSize;
	UINT32 frameRateNum;
	UINT32 frameRateDenom;
	UINT32 aspectRatioNum;
	UINT32 aspectRatioDenom;
	UINT32 sampleSize;
	UINT32 interlaceMode;
	GUID majorType; // video or audio
	GUID subType; // fourCC

	MediaType(IMFMediaType* pType = 0) :
		width(0), height(0),
		stride(0),
		isFixedSize(true),
		frameRateNum(1), frameRateDenom(1),
		aspectRatioNum(1), aspectRatioDenom(1),
		sampleSize(0),
		interlaceMode(0),
		majorType(MFMediaType_Video),
		subType({ 0 })
	{
		if (pType)
		{
			MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &width, &height);
			pType->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride); // value is stored as UINT32 but should be casted to INT3)
			pType->GetUINT32(MF_MT_FIXED_SIZE_SAMPLES, &isFixedSize);
			MFGetAttributeRatio(pType, MF_MT_FRAME_RATE, &frameRateNum, &frameRateDenom);
			MFGetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, &aspectRatioNum, &aspectRatioDenom);
			pType->GetUINT32(MF_MT_SAMPLE_SIZE, &sampleSize);
			pType->GetUINT32(MF_MT_INTERLACE_MODE, &interlaceMode);
			pType->GetGUID(MF_MT_MAJOR_TYPE, &majorType);
			pType->GetGUID(MF_MT_SUBTYPE, &subType);
		}
	}
	static MediaType createDefault()
	{
		MediaType res;
		res.width = 640;
		res.height = 480;
		res.setFramerate(30.0);
		return res;
	}
	inline bool isEmpty() const
	{
		return width == 0 && height == 0;
	}
	ComPtr<IMFMediaType> createMediaType() const
	{
		ComPtr<IMFMediaType> res;
		MFCreateMediaType(&res);
		if (width != 0 || height != 0)
			MFSetAttributeSize(res.Get(), MF_MT_FRAME_SIZE, width, height);
		if (stride != 0)
			res->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
		res->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, isFixedSize);
		if (frameRateNum != 0 || frameRateDenom != 0)
			MFSetAttributeRatio(res.Get(), MF_MT_FRAME_RATE, frameRateNum, frameRateDenom);
		if (aspectRatioNum != 0 || aspectRatioDenom != 0)
			MFSetAttributeRatio(res.Get(), MF_MT_PIXEL_ASPECT_RATIO, aspectRatioNum, aspectRatioDenom);
		if (sampleSize > 0)
			res->SetUINT32(MF_MT_SAMPLE_SIZE, sampleSize);
		res->SetUINT32(MF_MT_INTERLACE_MODE, interlaceMode);
		if (majorType != GUID())
			res->SetGUID(MF_MT_MAJOR_TYPE, majorType);
		if (subType != GUID())
			res->SetGUID(MF_MT_SUBTYPE, subType);
		return res;
	}
	void setFramerate(double fps)
	{
		frameRateNum = (UINT32)round(fps * 1000.0);
		frameRateDenom = 1000;
	}
	double getFramerate() const
	{
		return frameRateDenom != 0 ? ((double)frameRateNum) / ((double)frameRateDenom) : 0;
	}
	LONGLONG getFrameStep() const
	{
		const double fps = getFramerate();
		return (LONGLONG)(fps > 0 ? 1e7 / fps : 0);
	}
	inline unsigned long resolutionDiff(const MediaType& other) const
	{
		const unsigned long wdiff = absDiff(width, other.width);
		const unsigned long hdiff = absDiff(height, other.height);
		return wdiff + hdiff;
	}
	// check if 'this' is better than 'other' comparing to reference
	bool isBetterThan(const MediaType& other, const MediaType& ref) const
	{
		const unsigned long thisDiff = resolutionDiff(ref);
		const unsigned long otherDiff = other.resolutionDiff(ref);
		if (thisDiff < otherDiff)
			return true;
		if (thisDiff == otherDiff)
		{
			if (width > other.width)
				return true;
			if (width == other.width && height > other.height)
				return true;
			if (width == other.width && height == other.height)
			{
				const double thisRateDiff = absDiff(getFramerate(), ref.getFramerate());
				const double otherRateDiff = absDiff(other.getFramerate(), ref.getFramerate());
				if (thisRateDiff < otherRateDiff)
					return true;
			}
		}
		return false;
	}
};

#endif

