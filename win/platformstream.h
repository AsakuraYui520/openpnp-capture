/*

    OpenPnp-Capture: a video capture subsystem.

    Windows platform code

    Created by Niels Moseley on 7/6/17.
    Copyright © 2017 Niels Moseley. All rights reserved.

    Stream class

*/

#ifndef win32platform_stream_h
#define win32platform_stream_h

#include <windows.h>
#include <dshow.h>
#include <Vidcap.h>
#include <Ksmedia.h>

#include <stdint.h>
#include <vector>
#include <mutex>
#include "../common/logging.h"
#include "../common/stream.h"
#include "samplegrabber.h"


class Context;          // pre-declaration
class PlatformStream;  // pre-declaration

/** A class to handle callbacks from the video subsystem,
    A call is made for every frame.

    Note that the callback will be called by the DirectShow thread
    and should return as quickly as possible to avoid interference
    with the capturing process.
*/
class StreamCallbackHandler : public ISampleGrabberCB
{
public:
    StreamCallbackHandler(PlatformStream *stream) : m_stream(stream)
    {
        m_callbackCounter = 0;
    }

    ~StreamCallbackHandler()
    {
        LOG(LOG_INFO, "Callback counter = %d\n", m_callbackCounter);
    }

    /** callback handler used in this library */
    virtual HRESULT __stdcall SampleCB(double time, IMediaSample* sample) override;

    /** alternate callback handler (not used) */
    virtual HRESULT __stdcall BufferCB(double time, BYTE* buffer, long len) override
    {
        //m_callbackCounter++;
        return S_OK;
    }

    /** function implementation required by ISampleGrabberCB base class */
    virtual HRESULT __stdcall QueryInterface( REFIID iid, LPVOID *ppv )
    {
        if( iid == IID_ISampleGrabberCB || iid == IID_IUnknown )
        {
            *ppv = (void *) static_cast<ISampleGrabberCB*>( this );
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    /** function implementation required by ISampleGrabberCB base class */
    virtual ULONG	__stdcall AddRef()
    {
        return 1;
    }

    /** function implementation required by ISampleGrabberCB base class */
    virtual ULONG	__stdcall Release()
    {
        return 2;
    }

    uint32_t getCallbackCounter() const
    {
        return m_callbackCounter;
    }

    void reset()
    {
        m_callbackCounter = 0;
    }


private:
    PlatformStream* m_stream;
    uint32_t        m_callbackCounter;
};


/** The stream class handles the capturing of a single device */
class PlatformStream : public Stream
{
friend StreamCallbackHandler;

public:
    PlatformStream();
    ~PlatformStream();

    /** Open a capture stream to a device and request a specific (internal) stream format. 
        When succesfully opened, capturing starts immediately.
    */
    virtual bool open(Context *owner, deviceInfo *device, uint32_t width, uint32_t height, uint32_t fourCC) override;

    /** Close a capture stream */
    virtual void close() override;

    /** Returns true if a new frame is available for reading using 'captureFrame'. 
        The internal new frame flag is reset by captureFrame.
    */
    bool hasNewFrame();

    /** Retrieve the most recently captured frame and copy it in a
        buffer pointed to by RGBbufferPtr. The maximum buffer size 
        must be supplied in RGBbufferBytes.
    */
    bool captureFrame(uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes);

    /** Return the FOURCC media type of the stream */
    virtual uint32_t getFOURCC() override;

    virtual bool setExposure(int32_t value) override;

    virtual bool setAutoExposure(bool enabled) override;

    virtual bool getExposureLimits(int32_t *min, int32_t *max) override;

protected:
    void dumpCameraProperties();

    IFilterGraph2*  m_graph;
    IMediaControl*  m_control;
    IBaseFilter*    m_sourceFilter;
    IBaseFilter*    m_sampleGrabberFilter;
    ISampleGrabber* m_sampleGrabber;
    ICaptureGraphBuilder2* m_capture;
    IAMCameraControl* m_camControl;

    /** Each time a new frame is available, the DirectShow subsystem
        will call the callback handler */
    StreamCallbackHandler *m_callbackHandler;

    VIDEOINFOHEADER m_videoInfo;            ///< video information of current captured stream
};

#endif