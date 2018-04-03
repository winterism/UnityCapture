/*
  Unity Capture
  Copyright (c) 2018 Bernhard Schelling

  Based on UnityCam
  https://github.com/mrayy/UnityCam
  Copyright (c) 2016 MHD Yamen Saraiji

  The MIT License (MIT)

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

#include "shared.inl"
#include "streams.h"
#include <cguid.h>
#include <chrono>
#include <functional>

#define CaptureSourceName L"Unity Video Capture"

//Use separate GUIDs for 64bit and 32bit so both can be installed at the same time
#ifdef _WIN64
DEFINE_GUID(CLSID_UnityCaptureService,    0x5c2cd55c, 0x92ad, 0x4999, 0x86, 0x66, 0x91, 0x2b, 0xd3, 0xe7, 0x00, 0x10);
DEFINE_GUID(CLSID_UnityCaptureProperties, 0x5c2cd55c, 0x92ad, 0x4999, 0x86, 0x66, 0x91, 0x2b, 0xd3, 0xe7, 0x00, 0x11);
#else
DEFINE_GUID(CLSID_UnityCaptureService,    0x5c2cd55c, 0x92ad, 0x4999, 0x86, 0x66, 0x91, 0x2b, 0xd3, 0xe7, 0x00, 0x20);
DEFINE_GUID(CLSID_UnityCaptureProperties, 0x5c2cd55c, 0x92ad, 0x4999, 0x86, 0x66, 0x91, 0x2b, 0xd3, 0xe7, 0x00, 0x21);
#endif

//List of resolutions offered by this filter
//See comment above about scaling and how to set the rendering resolution in Unity
//If you add a higher resolution, make sure to update MAX_SHARED_IMAGE_SIZE
static struct { int width, height; } _media[] =
{
	{ 1920, 1080 },
	{ 1280,  720 },
	{  640,  360 },
	{  256,  144 },
	{ 2560, 1440 },
	{ 3840, 2160 },
	{ 1440, 1080 },
	{  960,  720 },
	{  640,  480 },
	{  480,  360 },
	{  320,  240 },
	{  192,  144 },
	{ 1920, 1440 },
	{ 2880, 2160 },
};

//Error draw modes (what to display on screen in case of errors/warnings)
enum EErrorDrawCase { EDC_ResolutionMismatch, EDC_UnityNeverStarted, EDC_UnitySendingStopped, _EDC_MAX };
enum EErrorDrawMode { EDM_GREENKEY, EDM_BLUEPINK, EDM_GREENYELLOW, EDM_BLACK };
static EErrorDrawMode ErrorDrawModes[_EDC_MAX] = { EDM_BLUEPINK, EDM_GREENYELLOW, EDM_GREENKEY };
static wchar_t* ErrorDrawModeNames[] = { L"Green Key (RGB #00FE00)", L"Blue/Pink Pattern", L"Green/Yellow Pattern", L"Fill Black" };
static bool OutputFrameRate = false;

#ifdef _DEBUG
void DebugLog(const char *format, ...)
{
	size_t size = 1024;
	char stackbuf[1024], *buf = stackbuf;
	std::string dynamicbuf;
	for (va_list ap;;)
	{
		va_start(ap, format); int needed = vsnprintf_s(buf, size, size, format, ap); va_end(ap);
		if (needed >= 0 && needed <= (int)size) { OutputDebugStringA(buf); return; }
		size = (needed > 0) ? (needed+1) : (size*2);
		dynamicbuf.resize(size);
		buf = &dynamicbuf.at(0);
	}
}
#else
#define DebugLog(...) ((void)0)
#endif

//Interface definition for ICamSource used by CCaptureSource
DEFINE_GUID(IID_ICamSource, 0xdd20e647, 0xf3e5, 0x4156, 0xb3, 0x7b, 0x54, 0x6f, 0xcf, 0x88, 0xec, 0x50);
DECLARE_INTERFACE_(ICamSource, IUnknown) { };

class CCaptureStream : CSourceStream, IKsPropertySet, IAMStreamConfig, IAMStreamControl, IAMPushSource
{
public:
	CCaptureStream(CSource* pOwner, HRESULT* phr) : CSourceStream("Stream", phr, pOwner, L"Output")
	{
		m_llFrame = m_llFrameMissCount = 0;
		m_prevStartTime = 0;
		m_avgTimePerFrame = 10000000 / 30;
		m_pReceiver = new SharedImageMemory();
		m_iUnscaledBufSize = 0;
		m_pUnscaledBuf = NULL;
		GetMediaType(0, &m_mt);
	}

	virtual ~CCaptureStream()
	{
		delete m_pReceiver;
		if (m_pUnscaledBuf) delete[] m_pUnscaledBuf;
	}

private:
	HRESULT FillBuffer(IMediaSample *pSamp) override
	{
		HRESULT hr;
		BYTE* pBuf;
		VIDEOINFO *pvi = (VIDEOINFO*)m_mt.Format();
		REFERENCE_TIME startTime = m_prevStartTime, endTime = startTime + m_avgTimePerFrame;
		LONGLONG mtStart = m_llFrame, mtEnd = mtStart + 1;
		m_prevStartTime = endTime;
		m_llFrame = mtEnd;
		UCASSERT(pSamp->GetSize() == pvi->bmiHeader.biSizeImage);
		UCASSERT(DIBSIZE(pvi->bmiHeader) == pvi->bmiHeader.biSizeImage);

		if (FAILED(hr = pSamp->GetPointer(&pBuf))) return hr;
		if (FAILED(hr = pSamp->SetActualDataLength(pvi->bmiHeader.biSizeImage))) return hr;
		if (FAILED(hr = pSamp->SetTime(&startTime, &endTime))) return hr;
		if (FAILED(hr = pSamp->SetMediaTime(&mtStart, &mtEnd))) return hr;

		ProcessState State = { pBuf, pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight, this };
		switch (m_pReceiver->Receive((SharedImageMemory::ReceiveCallbackFunc)ProcessImage, &State))
		{
			case SharedImageMemory::RECEIVERES_CAPTUREINACTIVE:{
				//Show color pattern indicating that Unity is not sending frame data yet
				char DisplayString[] = "Unity has not started sending image data", *DisplayStrings[] = { DisplayString };
				int DisplayStringLens[] = { sizeof(DisplayString) - 1 };
				FillErrorPattern(ErrorDrawModes[EDC_UnityNeverStarted], &State, 1, DisplayStrings, DisplayStringLens, m_llFrame);
				Sleep((DWORD)(m_avgTimePerFrame / 10000 - 1)); //just wait a bit until capturing next frame
				break;}

			case SharedImageMemory::RECEIVERES_NEWFRAME:
				if (m_llFrameMissCount) m_llFrameMissCount = 0;
				break;

			case SharedImageMemory::RECEIVERES_OLDFRAME:{
				if (m_llFrameMissCount++ < 5) break;
				//Show color pattern when having 5 frames without new image (probably Unity stopped sending data)
				char DisplayString[] = "Unity has stopped sending image data", *DisplayStrings[] = { DisplayString };
				int DisplayStringLens[] = { sizeof(DisplayString) - 1 };
				FillErrorPattern(ErrorDrawModes[EDC_UnitySendingStopped], &State, 1, DisplayStrings, DisplayStringLens, m_llFrame);
				break;}
		}
		if (OutputFrameRate) RenderFPSDisplay(&State);
		return S_OK;
	}

	struct ProcessJob
	{
		enum EType { JOB_NONE, JOB_RGBA8toRGB8, JOB_RGBA16toRGB8, JOB_RESIZE_LINEAR, JOB_MIRROR_HORIZONTAL } Type;
		const void *BufIn; void *BufOut;
		size_t Width, RowStart, RowEnd, RGBAInStride, ResizeToHeight, ResizeFromWidth, ResizeFromHeight;

		inline void Execute()
		{
			if      (Type == JOB_RGBA8toRGB8)       RGBA8toRGB8();
			else if (Type == JOB_RGBA16toRGB8)      RGBA16toRGB8();
			else if (Type == JOB_RESIZE_LINEAR)     ResizeLinear();
			else if (Type == JOB_MIRROR_HORIZONTAL) MirrorHorizontal();
		}

		void RGBA8toRGB8()
		{
			const uint32_t *src = (const uint32_t*)BufIn + (RowStart * RGBAInStride);
			uint8_t *dst = (uint8_t*)BufOut + (RowStart * Width * 3), *dstEnd = (uint8_t*)BufOut + (RowEnd * Width * 3);
			if (RGBAInStride != Width)
			{
				//Handle a case where the texture pitch does have a gap on the right side
				const uint32_t *srcLastRow = (const uint32_t*)BufIn + ((RowEnd - 1) * RGBAInStride);
				for (size_t dstPitch = Width * 3, srcStride = RGBAInStride, iMax = Width; src != srcLastRow; dst += dstPitch, src += srcStride)
					for (size_t i = 0; i != iMax; i++, dst += 3)
						*(uint32_t*)dst = _byteswap_ulong(src[i]) >> 8;
				for (size_t i = 0, iMax = Width - 1; i != iMax; i++, dst += 3, src++)
					*(uint32_t*)dst = _byteswap_ulong(*src) >> 8;
			}
			else
			{
				//The fastest (implemented) path to convert from RGBA to BGR
				const uint32_t *srcEnd8 = src + (((RowEnd-RowStart)*Width-1)&~7), *srcEnd1 = src + ((RowEnd-RowStart)*Width-1);
				for (; src != srcEnd8; dst += 24, src += 8)
				{
					*(uint32_t*)(dst     ) = _byteswap_ulong(src[0]) >> 8;
					*(uint32_t*)(dst +  3) = _byteswap_ulong(src[1]) >> 8;
					*(uint32_t*)(dst +  6) = _byteswap_ulong(src[2]) >> 8;
					*(uint32_t*)(dst +  9) = _byteswap_ulong(src[3]) >> 8;
					*(uint32_t*)(dst + 12) = _byteswap_ulong(src[4]) >> 8;
					*(uint32_t*)(dst + 15) = _byteswap_ulong(src[5]) >> 8;
					*(uint32_t*)(dst + 18) = _byteswap_ulong(src[6]) >> 8;
					*(uint32_t*)(dst + 21) = _byteswap_ulong(src[7]) >> 8;
				}
				for (; src != srcEnd1; dst += 3, src++)
					*(uint32_t*)(dst) = _byteswap_ulong(*src) >> 8;
			}
			uint32_t FinalPixel = _byteswap_ulong(*src) >> 8;
			memcpy(dst, &FinalPixel, 3);
		}

		void RGBA16toRGB8()
		{
			//16 bit color downscaling (HDR (16 bit floats) to BGR)
			float tmpf; uint16_t *tmpp, tmpr, tmpg, tmpb;
			#define F16toU8(s) ((s) & 0x8000 ? 0 : (*(uint32_t*)&tmpf = ((s) << 13) + 0x38000000, tmpf < 1.0f ? (int)(tmpf * 255.99f) : 255))
			#define RGBAF16toBGRU8(psrc) (tmpp = (uint16_t*)(psrc), tmpr = tmpp[0], tmpg = tmpp[1], tmpb = tmpp[2], (F16toU8(tmpr)<<16) | (F16toU8(tmpg)<<8) | F16toU8(tmpb))
			const uint64_t *src = (const uint64_t*)BufIn + (RowStart * RGBAInStride);
			uint8_t *dst = (uint8_t*)BufOut + (RowStart * Width * 3), *dstEnd = (uint8_t*)BufOut + (RowEnd * Width * 3);
			if (RGBAInStride != Width)
			{
				//Handle a case where the texture pitch does have a gap on the right side
				const uint64_t *srcLastRow = (const uint64_t*)BufIn + ((RowEnd - 1) * RGBAInStride);
				for (size_t dstPitch = Width * 3, srcStride = RGBAInStride, iMax = Width; src != srcLastRow; dst += dstPitch, src += srcStride)
					for (size_t i = 0; i != iMax; i++, dst += 3)
						*(uint32_t*)dst = RGBAF16toBGRU8(src + i);
				for (size_t i = 0, iMax = Width - 1; i != iMax; i++, dst += 3, src++)
					*(uint32_t*)dst = RGBAF16toBGRU8(src);
			}
			else
			{
				//The fastest (implemented) path to convert from RGBA to BGR
				const uint64_t *srcEnd8 = src + (((RowEnd-RowStart)*Width-1)&~7), *srcEnd1 = src + ((RowEnd-RowStart)*Width-1);
				for (; src != srcEnd8; dst += 24, src += 8)
				{
					*(uint32_t*)(dst     ) = RGBAF16toBGRU8(src    );
					*(uint32_t*)(dst +  3) = RGBAF16toBGRU8(src + 1);
					*(uint32_t*)(dst +  6) = RGBAF16toBGRU8(src + 2);
					*(uint32_t*)(dst +  9) = RGBAF16toBGRU8(src + 3);
					*(uint32_t*)(dst + 12) = RGBAF16toBGRU8(src + 4);
					*(uint32_t*)(dst + 15) = RGBAF16toBGRU8(src + 5);
					*(uint32_t*)(dst + 18) = RGBAF16toBGRU8(src + 6);
					*(uint32_t*)(dst + 21) = RGBAF16toBGRU8(src + 7);
				}
				for (; src != srcEnd1; dst += 3, src++)
					*(uint32_t*)(dst) = RGBAF16toBGRU8(src);
			}
			//For the final pixel we can't use 4 byte uint32_t copy so we call memcpy
			uint32_t FinalPixel = RGBAF16toBGRU8(src);
			memcpy(dst, &FinalPixel, 3);
			#undef F16toU8
			#undef RGBAF16toBGRU8
		}

		void ResizeLinear()
		{
			const size_t w = Width, h = ResizeToHeight, pitch = w * 3, ResizeFromPitch = ResizeFromWidth * 3;
			const double aw = (double)w, ah = (double)h;
			const double scale = max(ResizeFromWidth / aw, ResizeFromHeight / ah);
			const double ax = (aw - (ResizeFromWidth  / scale)) / 2.0;
			const double ay = (ah - (ResizeFromHeight / scale)) / 2.0;
			const uint8_t *src = (const uint8_t*)BufIn, BlackPixel[3] = {0, 0, 0};
			uint8_t *dst = (uint8_t*)BufOut + (RowStart * Width * 3);
			for (size_t y = RowStart, yEnd = RowEnd, isOffsetMax = ResizeFromHeight * ResizeFromPitch; y != RowEnd; y++)
				for (size_t x = 0; x != w; x++, dst += 3)
				{
					size_t isx = (size_t)((x-ax)*scale), isy = (size_t)((y-ay)*scale), isOffset = isy * ResizeFromPitch + isx * 3;
					memcpy(dst, (isOffset >= isOffsetMax ? BlackPixel : src + isOffset), 3);
				}
		}

		void MirrorHorizontal()
		{
			uint8_t *dst = (uint8_t*)BufOut + (RowStart * Width * 3), *dstEnd = (uint8_t*)BufOut + (RowEnd * Width * 3);
			for (size_t dstPitch = Width * 3; dst != dstEnd; dst += dstPitch)
				for (uint8_t tmp[3], *dstA = dst, *dstB = dst + dstPitch - 3; dstA < dstB; dstA += 3, dstB -= 3)
					memcpy(tmp, dstA, 3), memcpy(dstA, dstB, 3), memcpy(dstB, tmp, 3);
		}
	};

	struct ProcessWorkers
	{
		ProcessWorkers() : WorkersRunning(WORKERCOUNT)
		{
			for (size_t i = 0; i != WORKERCOUNT; i++) Threads[i].Start((sThread::FUNC_t)&ProcessThread, (void*)this);
		}

		~ProcessWorkers()
		{
			WorkersRunning = 0;
			for (size_t i = 0; i != WORKERCOUNT; i++) NewJobSemaphore.Post(); //wake up all threads
		}
	
		void StartNewJob(ProcessJob NewJob)
		{
			//Notify threads of new work to do
			WorkingJobCount = 0;
			size_t Num = NewJob.RowEnd;
			for (size_t i = 0; i != WORKERCOUNT; i++)
			{
				NewJob.RowStart = Num * (i  ) / (WORKERCOUNT + 1);
				NewJob.RowEnd   = Num * (i+1) / (WORKERCOUNT + 1);
				Jobs[i] = NewJob;
				NewJobSemaphore.Post();
			}

			//Do work in the main thread as well
			NewJob.RowStart = NewJob.RowEnd;
			NewJob.RowEnd   = Num;
			NewJob.Execute();

			//Wait for threads to finish working
			for (size_t i = 0; i != WORKERCOUNT && JobDoneSemaphore.WaitForPost(); i++) {}
		}

	private:
		//Wrapper objects for Windows concurrency objects (thread, mutex, semaphore)
		struct sThread { typedef DWORD (WINAPI *FUNC_t)(LPVOID); sThread() : h(0) {} sThread(FUNC_t f, void* p = NULL) : h(0) { Start(f, p); } void Start(FUNC_t f, void* p = NULL) { if (h) this->~sThread(); h = CreateThread(0,0,f,p,0,0); } ~sThread() { if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); } } private:HANDLE h;sThread(const sThread&);sThread& operator=(const sThread&);};
		struct sMutex { sMutex() : h(CreateMutexA(0,0,0)) {} ~sMutex() { CloseHandle(h); } __inline void Lock() { WaitForSingleObject(h,INFINITE); } __inline void Unlock() { ReleaseMutex(h); } private:HANDLE h;sMutex(const sMutex&);sMutex& operator=(const sMutex&);};
		struct sSemaphore { sSemaphore() : h(CreateSemaphoreA(0,0,32768,0)) {} ~sSemaphore() { CloseHandle(h); } __inline void Post() { ReleaseSemaphore(h, 1, 0); } __inline bool WaitForPost() { return WaitForSingleObject(h,INFINITE) == WAIT_OBJECT_0; } private:HANDLE h;sSemaphore(const sSemaphore&);sSemaphore& operator=(const sSemaphore&);};

		enum { WORKERCOUNT = 3 };
		sMutex JobsMutex;
		sThread Threads[WORKERCOUNT];
		ProcessJob Jobs[WORKERCOUNT];
		sSemaphore NewJobSemaphore, JobDoneSemaphore;
		size_t WorkingJobCount, WorkersRunning;

		static void ProcessThread(ProcessWorkers* mw)
		{
			while (mw->NewJobSemaphore.WaitForPost() && mw->WorkersRunning)
			{
				mw->JobsMutex.Lock();
				size_t MyJob = mw->WorkingJobCount++;
				mw->JobsMutex.Unlock();
				mw->Jobs[MyJob].Execute();
				mw->JobDoneSemaphore.Post();
			}
		}
	};

	struct ProcessState
	{
		uint8_t* Buf;
		int BufWidth, BufHeight;
		CCaptureStream* Owner;
	};

	static void ProcessImage(int InWidth, int InHeight, int InStride, int InRGBABits, SharedImageMemory::EResizeMode ResizeMode, SharedImageMemory::EMirrorMode MirrorMode, uint8_t* InBuf, ProcessState* State)
	{
		const bool NeedResize = (InWidth != State->BufWidth || InHeight != State->BufHeight);
		if (NeedResize && ResizeMode == SharedImageMemory::RESIZEMODE_DISABLED)
		{
			//Show color pattern indicating that the requested resolution does not match the resolution provided by Unity
			char DisplayString1[128], DisplayString2[128], DisplayString3[128];
			char* DisplayStrings[] = { DisplayString1, DisplayString2, DisplayString3 };
			int DisplayStringLens[] = {
				sprintf_s(DisplayString1, sizeof(DisplayString1), "Capture output resolution is %d x %d", State->BufWidth, State->BufHeight),
				sprintf_s(DisplayString2, sizeof(DisplayString2), "Unity render resolution is %d x %d", InWidth, InHeight),
				sprintf_s(DisplayString3, sizeof(DisplayString3), "please set these to match"),
			};
			FillErrorPattern(ErrorDrawModes[EDC_ResolutionMismatch], State, 3, DisplayStrings, DisplayStringLens);
			return;
		}

		if (NeedResize)
		{
			//Prepare buffer for image scaling
			DWORD UnscaledBufSize = (InWidth * InHeight * 3);
			if (State->Owner->m_iUnscaledBufSize != UnscaledBufSize)
			{
				if (State->Owner->m_pUnscaledBuf) delete[] State->Owner->m_pUnscaledBuf;
				State->Owner->m_pUnscaledBuf = new uint8_t[UnscaledBufSize];
				State->Owner->m_iUnscaledBufSize = UnscaledBufSize;
			}
		}

		//Multi-threaded conversion of RGBA source to 8-bit BGR format while also eliminating possible row gaps (when stride != width)
		UCASSERT(InRGBABits == 8 || InRGBABits == 16);
		ProcessJob Job;
		Job.Type = (InRGBABits == 8 ? ProcessJob::JOB_RGBA8toRGB8 : ProcessJob::JOB_RGBA16toRGB8);
		Job.BufIn = InBuf, Job.BufOut = (NeedResize ? State->Owner->m_pUnscaledBuf : State->Buf);
		Job.Width = InWidth, Job.RowStart = 0, Job.RowEnd = InHeight, Job.RGBAInStride = InStride;
		State->Owner->m_ProcessWorkers.StartNewJob(Job);

		if (NeedResize)
		{
			//Multi-threaded image scaling
			Job.Type = ProcessJob::JOB_RESIZE_LINEAR;
			Job.BufIn = State->Owner->m_pUnscaledBuf, Job.BufOut = State->Buf;
			Job.Width = State->BufWidth, Job.RowStart = 0, Job.RowEnd = State->BufHeight;
			Job.ResizeToHeight = State->BufHeight, Job.ResizeFromWidth = InWidth, Job.ResizeFromHeight = InHeight;
			State->Owner->m_ProcessWorkers.StartNewJob(Job);
		}

		if (MirrorMode == SharedImageMemory::MIRRORMODE_HORIZONTALLY)
		{
			//Multi-threaded horizontal image flipping
			Job.Type = ProcessJob::JOB_MIRROR_HORIZONTAL;
			Job.BufOut = State->Buf;
			Job.Width = State->BufWidth, Job.RowStart = 0, Job.RowEnd = State->BufHeight;
			State->Owner->m_ProcessWorkers.StartNewJob(Job);
		}
	}

	static void FillErrorPattern(EErrorDrawMode edm, ProcessState* State, int LineCount = 0, char** LineStrings = NULL, int* LineLengths = NULL, LONGLONG FrameNumber = -1)
	{
		if (FrameNumber >= 0 && FrameNumber < 5) edm = EDM_BLACK; //show errors as just black during the first 5 frames (when starting)
		BYTE *p = State->Buf, *pEnd = State->Buf + (State->BufWidth * State->BufHeight * 3);
		switch (edm)
		{
			case EDM_GREENKEY:    while (p != pEnd) { *(p++) = 0x00; *(p++) = 0xFE; *(p++) = 0x00; } break; //Filled with 0x00FE00 (BGR colors)
			case EDM_GREENYELLOW: while (p != pEnd) { *(p++) = 0x00; *(p++) = 0xFF; *(p++) = (size_t)p%0xFF; } break; //Green/yellow color pattern (BGR colors)
			case EDM_BLUEPINK:    while (p != pEnd) { *(p++) = 0xFF; *(p++) = 0x00; *(p++) = (size_t)p%0xFF; } break; //Blue/pink color pattern (BGR colors)
			case EDM_BLACK:       ZeroMemory(State->Buf, (State->BufWidth * State->BufHeight * 3)); break; //Filled with black
		}

		if (LineCount && edm != EDM_BLACK && edm != EDM_GREENKEY)
		{
			void* pTextBuf;
			HDC TextDC = CreateCompatibleDC(0);
			BITMAPINFO TextBMI = { sizeof(BITMAPINFOHEADER), State->BufWidth, LineCount * 20, 1, 8 * 3, 0, LineCount * 20 * State->BufWidth * 3 };
			TextBMI.bmiHeader.biHeight = LineCount * 20;
			HBITMAP TextHBitmap = CreateDIBSection(TextDC, &TextBMI, DIB_RGB_COLORS, &pTextBuf, NULL, 0);
			SelectObject(TextDC, TextHBitmap);
			SetBkMode(TextDC, TRANSPARENT);
			SetTextColor(TextDC, RGB(255, 0, 0));
			for (int i = 0; i < LineCount; i++) TextOutA(TextDC, 10,  i * 20, LineStrings[i], LineLengths[i]);
			memcpy(State->Buf + ((State->BufHeight - TextBMI.bmiHeader.biHeight) / 2) * State->BufWidth * 3, pTextBuf, TextBMI.bmiHeader.biHeight * State->BufWidth * 3);
			DeleteObject(TextHBitmap);
			DeleteDC(TextDC);
		}
	}

	static void RenderFPSDisplay(ProcessState* State)
	{
		static LONGLONG MyFPS = 0, MyLastFPSTime = GetTickCount64(), MyLastFPS = 0;
		for (MyFPS++; GetTickCount64() - MyLastFPSTime > 1000; MyFPS = 0, MyLastFPSTime += 1000) { MyLastFPS = MyFPS; }
		char DisplayString[128];
		int DisplayStringLen = sprintf_s(DisplayString, sizeof(DisplayString), "%d FPS", MyLastFPS);

		void* pTextBuf;
		HDC TextDC = CreateCompatibleDC(0);
		BITMAPINFO TextBMI = { sizeof(BITMAPINFOHEADER), State->BufWidth, 20, 1, 8 * 3, 0, 20 * State->BufWidth * 3 };
		HBITMAP TextHBitmap = CreateDIBSection(TextDC, &TextBMI, DIB_RGB_COLORS, &pTextBuf, NULL, 0);
		SelectObject(TextDC, TextHBitmap);
		SetBkMode(TextDC, TRANSPARENT);
		SetTextColor(TextDC, RGB(0, 255, 0));
		TextOutA(TextDC, 10, 0, DisplayString, DisplayStringLen);
		memcpy(State->Buf, pTextBuf, TextBMI.bmiHeader.biHeight * State->BufWidth * 3);
		DeleteObject(TextHBitmap);
		DeleteDC(TextDC);
	}

	//IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == NULL) return E_POINTER;
		else if (riid == _uuidof(IAMStreamConfig)) { *ppv = (IAMStreamConfig*)this; AddRef(); return S_OK; }
		else if (riid == _uuidof(IKsPropertySet))  { *ppv = (IKsPropertySet*)this;  AddRef(); return S_OK; }
		return CSourceStream::QueryInterface(riid, ppv);
	}

	STDMETHODIMP_(ULONG) AddRef() override  { return GetOwner()->AddRef();  }
	STDMETHODIMP_(ULONG) Release() override { return GetOwner()->Release(); }

	STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void ** ppv) override
	{
		if (ppv == NULL) return E_POINTER;
		else if (riid == IID_IKsPropertySet)  { *ppv = (IKsPropertySet*)this;  AddRef(); return S_OK; }
		else if (riid == IID_IQualityControl) { *ppv = (IQualityControl*)this; AddRef(); return S_OK; }
		else if (riid == IID_IAMStreamConfig) { *ppv = (IAMStreamConfig*)this; AddRef(); return S_OK; }
		return CSourceStream::NonDelegatingQueryInterface(riid, ppv);
	}

	STDMETHODIMP QuerySupported(REFGUID rguidPropSet, ULONG ulId, PULONG pulTypeSupport) override
	{
		if (rguidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
		if (ulId != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
		if (pulTypeSupport) *pulTypeSupport = KSPROPERTY_SUPPORT_GET; // We support getting this property, but not setting it.
		return S_OK;

		//if(rguidPropSet == AMPROPSETID_Pin && ulId == AMPROPERTY_PIN_CATEGORY) { *pulTypeSupport = KSPROPERTY_SUPPORT_GET; return S_OK; }
		//return E_NOTIMPL;
	}

	STDMETHODIMP Get(REFGUID rguidPropSet, ULONG ulId, LPVOID pInstanceData, ULONG ulInstanceLength, LPVOID pPropertyData, ULONG ulDataLength, PULONG pulBytesReturned) override
	{
		if (rguidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
		if (ulId != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
		if (pPropertyData == NULL && pulBytesReturned == NULL) return E_POINTER;

		if (pulBytesReturned) *pulBytesReturned = sizeof(GUID);
		if (pPropertyData == NULL) return S_OK; // Caller just wants to know the size. 
		if (ulDataLength < sizeof(GUID)) return E_UNEXPECTED; // The buffer is too small.

		*(GUID *)pPropertyData = PIN_CATEGORY_CAPTURE;
		return S_OK;

		//if(rguidPropSet == AMPROPSETID_Pin && ulId == AMPROPERTY_PIN_CATEGORY)
		//{
		//	if (pPropertyData == NULL) return E_POINTER;
		//	if (ulDataLength != sizeof(GUID)) return E_INVALIDARG;
		//	memcpy(pPropertyData, &PIN_CATEGORY_CAPTURE, sizeof(GUID));
		//	*pulBytesReturned = sizeof(GUID);
		//	return S_OK;
		//}
		//return E_NOTIMPL;
	}

	STDMETHODIMP Set(REFGUID rguidPropSet, ULONG ulId, LPVOID pInstanceData, ULONG ulInstanceLength, LPVOID pPropertyData, ULONG ulDataLength) override { return E_NOTIMPL; }
	STDMETHODIMP Notify(IBaseFilter *pSelf, Quality q) override { return S_OK; }
	STDMETHODIMP SetSink(IQualityControl *piqc) override { return S_OK; }

	HRESULT DecideBufferSize(IMemAllocator * pAlloc, ALLOCATOR_PROPERTIES * pRequest) override
	{
		if (pAlloc == NULL || pRequest == NULL) DebugLog("[DecideBufferSize] E_POINTER\n");
		if (pAlloc == NULL || pRequest == NULL) return E_POINTER;
		CAutoLock cAutoLock(m_pFilter->pStateLock());
		HRESULT hr = NOERROR;
		VIDEOINFO *pvi = (VIDEOINFO*)m_mt.Format();
		pRequest->cBuffers = 1;

		DebugLog("[DecideBufferSize] Request Size: %d - Have Size: %d\n", (int)pvi->bmiHeader.biSizeImage, (int)pRequest->cbBuffer);
		if (pvi->bmiHeader.biSizeImage > (DWORD)pRequest->cbBuffer)
			pRequest->cbBuffer = pvi->bmiHeader.biSizeImage;

		ALLOCATOR_PROPERTIES actual;
		hr = pAlloc->SetProperties(pRequest, &actual);
		if (FAILED(hr)) DebugLog("[DecideBufferSize] E_SOMETHING\n");
		if (FAILED(hr)) return hr;

		DebugLog("[DecideBufferSize] Request Size: %d - Actual Size: %d\n", (int)pvi->bmiHeader.biSizeImage, (int)actual.cbBuffer);
		return (actual.cbBuffer < pRequest->cbBuffer ? E_FAIL : S_OK);
	}

	STDMETHODIMP SetFormat(AM_MEDIA_TYPE *pmt) override
	{
		if (pmt == NULL) DebugLog("[SetFormat] E_POINTER\n");
		if (pmt == NULL) return E_POINTER;

		VIDEOINFO* pvi = (VIDEOINFO*)pmt->pbFormat;
		if (pvi == NULL) DebugLog("[SetFormat] E_UNEXPECTED\n");
		if (pvi == NULL) return E_UNEXPECTED;

		DebugLog("[SetFormat] WIDTH: %d - HEIGHT: %d - TPS: %d - SIZE: %d - SIZE CALC: %d\n", (int)pvi->bmiHeader.biWidth, (int)pvi->bmiHeader.biHeight, (int)pvi->AvgTimePerFrame,
			(int)pvi->bmiHeader.biSizeImage, (int)DIBSIZE(pvi->bmiHeader));
		if (pvi->bmiHeader.biSizeImage != DIBSIZE(pvi->bmiHeader)) return E_FAIL;
		m_avgTimePerFrame = pvi->AvgTimePerFrame;
		m_mt = *pmt;
		return S_OK;
	}

	STDMETHODIMP GetFormat(AM_MEDIA_TYPE **ppmt) override
	{
		if (ppmt == NULL) DebugLog("[GetFormat] E_POINTER\n");
		if (ppmt == NULL) return E_POINTER;
		DebugLog("[GetFormat] RETURNING WIDTH: %d - HEIGHT: %d - TPS: %d - SIZEIMAGE: %d - SIZECALC: %d\n", (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biWidth, (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biHeight, (int)((VIDEOINFO*)m_mt.Format())->AvgTimePerFrame, (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biSizeImage, (int)DIBSIZE(((VIDEOINFO*)m_mt.Format())->bmiHeader));
		*ppmt = CreateMediaType(&m_mt);
		return S_OK;
	}

	STDMETHODIMP GetNumberOfCapabilities(int *piCount, int *piSize) override
	{
		if (piCount == NULL || piSize == NULL) DebugLog("[GetNumberOfCapabilities] E_POINTER\n");
		if (piCount == NULL || piSize == NULL) return E_POINTER;
		*piCount = (sizeof(_media)/sizeof(_media[0]));
		*piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
		DebugLog("[GetNumberOfCapabilities] Returning Count: %d - Size: %d\n", *piCount, *piSize);
		return S_OK;
	}

	STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE **ppmt, BYTE *pSCC) override
	{
		if (ppmt == NULL || pSCC == NULL) DebugLog("[GetStreamCaps] E_POINTER\n");
		if (ppmt == NULL || pSCC == NULL) return E_POINTER;

		CMediaType mt;
		HRESULT hr = GetMediaType(iIndex, &mt);
		if (FAILED(hr)) return hr;
		VIDEOINFO *pvi = (VIDEOINFO*)mt.Format();
		DebugLog("[GetStreamCaps] Index: %d - WIDTH: %d - HEIGHT: %d - TPS: %d - SIZEIMAGE: %d - SIZECALC: %d\n", iIndex, (int)pvi->bmiHeader.biWidth, (int)pvi->bmiHeader.biHeight, (int)pvi->AvgTimePerFrame, (int)pvi->bmiHeader.biSizeImage, (int)DIBSIZE(pvi->bmiHeader));

		*ppmt = CreateMediaType(&mt);

		VIDEO_STREAM_CONFIG_CAPS* pCaps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
		ZeroMemory(pCaps, sizeof(VIDEO_STREAM_CONFIG_CAPS));

		pCaps->guid = FORMAT_VideoInfo;
		pCaps->VideoStandard      = 0;
		pCaps->CropAlignX         = 1;
		pCaps->CropAlignY         = 1;
		pCaps->OutputGranularityX = 1;
		pCaps->OutputGranularityY = 1;
		pCaps->StretchTapsX       = 0;
		pCaps->StretchTapsY       = 0;
		pCaps->ShrinkTapsX        = 0;
		pCaps->ShrinkTapsY        = 0;
		pCaps->InputSize.cx       = pvi->bmiHeader.biWidth;
		pCaps->InputSize.cy       = pvi->bmiHeader.biHeight;
		pCaps->MinCroppingSize.cx = pvi->bmiHeader.biWidth;
		pCaps->MinCroppingSize.cy = pvi->bmiHeader.biHeight;
		pCaps->MaxCroppingSize.cx = pvi->bmiHeader.biWidth;
		pCaps->MaxCroppingSize.cy = pvi->bmiHeader.biHeight;
		pCaps->CropGranularityX   = pvi->bmiHeader.biWidth;
		pCaps->CropGranularityY   = pvi->bmiHeader.biHeight;
		pCaps->MinOutputSize.cx   = pvi->bmiHeader.biWidth;
		pCaps->MinOutputSize.cy   = pvi->bmiHeader.biHeight;
		pCaps->MaxOutputSize.cx   = pvi->bmiHeader.biWidth;
		pCaps->MaxOutputSize.cy   = pvi->bmiHeader.biHeight;
		pCaps->MinFrameInterval = 10000000 / 120;
		pCaps->MaxFrameInterval = 10000000 / 30;
		pCaps->MinBitsPerSecond = pCaps->MinOutputSize.cx * pCaps->MinOutputSize.cy * 3 * 8 * 30;
		pCaps->MaxBitsPerSecond = pCaps->MaxOutputSize.cx * pCaps->MaxOutputSize.cy * 3 * 8 * 120;
		return S_OK;
	}

	HRESULT SetMediaType(const CMediaType *pmt) override
	{
		VIDEOINFOHEADER* pvi = (VIDEOINFOHEADER*)(pmt->Format());
		DebugLog("[SetMediaType] WIDTH: %d - HEIGHT: %d - TPS: %d - SIZEIMAGE: %d - SIZECALC: %d\n", (int)pvi->bmiHeader.biWidth, (int)pvi->bmiHeader.biHeight, (int)pvi->AvgTimePerFrame, (int)pvi->bmiHeader.biSizeImage, (int)DIBSIZE(pvi->bmiHeader));
		HRESULT hr = CSourceStream::SetMediaType(pmt);
		return hr;
	}

	HRESULT CheckMediaType(const CMediaType *pMediaType) override
	{
		CAutoLock lock(m_pFilter->pStateLock());
		VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER *)(pMediaType->Format());
		if (!pvi) DebugLog("[CheckMediaType] WANT VIDEO INFO NULL\n");
		else DebugLog("[CheckMediaType] [WANT] WIDTH: %d - HEIGHT: %d - TPS: %d - SIZEIMAGE: %d - SIZECALC: %d\n", (int)pvi->bmiHeader.biWidth, (int)pvi->bmiHeader.biHeight, (int)pvi->AvgTimePerFrame, (int)pvi->bmiHeader.biSizeImage, (int)DIBSIZE(pvi->bmiHeader));
		     DebugLog("[CheckMediaType] [HAVE] WIDTH: %d - HEIGHT: %d - TPS: %d - SIZEIMAGE: %d - SIZECALC: %d\n", (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biWidth, (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biHeight, (int)((VIDEOINFO*)m_mt.Format())->AvgTimePerFrame, (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biSizeImage, (int)DIBSIZE(((VIDEOINFO*)m_mt.Format())->bmiHeader));
		     DebugLog("[CheckMediaType] [RETURNING] %s\n", (*pMediaType != m_mt ? "E_INVALIDARG" : "S_OK"));
		return (*pMediaType != m_mt ? E_INVALIDARG : S_OK);
	}

	HRESULT GetMediaType(int iPos, CMediaType *pMediaType) override
	{
		CheckPointer(pMediaType, E_POINTER);
		if (iPos < 0) return E_INVALIDARG;
		if (iPos > (sizeof(_media)/sizeof(_media[0]))) return VFW_S_NO_MORE_ITEMS;
		CAutoLock cAutoLock(m_pFilter->pStateLock()); 

		VIDEOINFO *pvi = (VIDEOINFO *)pMediaType->AllocFormatBuffer(sizeof(VIDEOINFO));
		ZeroMemory(pvi, sizeof(VIDEOINFO));
		pvi->AvgTimePerFrame = m_avgTimePerFrame;
		BITMAPINFOHEADER *pBmi = &(pvi->bmiHeader);
		pBmi->biSize = sizeof(BITMAPINFOHEADER);
		pBmi->biWidth = _media[iPos].width;
		pBmi->biHeight = _media[iPos].height;
		pBmi->biPlanes = 1;
		pBmi->biBitCount = 24;
		pBmi->biCompression = BI_RGB;
		pvi->bmiHeader.biSizeImage = DIBSIZE(pvi->bmiHeader);

		//DebugLog("[GetMediaType] iPos: %d - WIDTH: %d - HEIGHT: %d - TPS: %d\n", iPos, (int)pvi->bmiHeader.biWidth, (int)pvi->bmiHeader.biHeight, (int)pvi->AvgTimePerFrame);

		pMediaType->SetType(&MEDIATYPE_Video);
		pMediaType->SetFormatType(&FORMAT_VideoInfo);
		const GUID subtype = GetBitmapSubtype(&pvi->bmiHeader);
		pMediaType->SetSubtype(&subtype);
		pMediaType->SetSampleSize(DIBSIZE(*pBmi));
		pMediaType->SetTemporalCompression(FALSE);
		return S_OK;
	}
		
	HRESULT OnThreadStartPlay() override
	{
		DebugLog("[OnThreadStartPlay] OnThreadStartPlay\n");
		m_llFrame = m_llFrameMissCount = 0;
		return CSourceStream::OnThreadStartPlay();
	}

	CMediaType m_mt;
	LONGLONG m_llFrame, m_llFrameMissCount;
	REFERENCE_TIME m_prevStartTime;
	REFERENCE_TIME m_avgTimePerFrame;
	SharedImageMemory* m_pReceiver;
	ProcessWorkers m_ProcessWorkers;
	DWORD m_iUnscaledBufSize;
	BYTE* m_pUnscaledBuf;

	//IAMStreamControl
	HRESULT STDMETHODCALLTYPE StartAt(const REFERENCE_TIME *ptStart, DWORD dwCookie) override { return NOERROR; }
	HRESULT STDMETHODCALLTYPE StopAt(const REFERENCE_TIME *ptStop, BOOL bSendExtra, DWORD dwCookie) override { return NOERROR; }
	HRESULT STDMETHODCALLTYPE GetInfo(AM_STREAM_INFO *pInfo) override { return NOERROR; }

	// IAMPushSource
	HRESULT STDMETHODCALLTYPE GetLatency(REFERENCE_TIME *prtLatency) override { return NOERROR; }
	HRESULT STDMETHODCALLTYPE GetPushSourceFlags(ULONG *pFlags) override { *pFlags = AM_PUSHSOURCECAPS_INTERNAL_RM; return NOERROR; }
	HRESULT STDMETHODCALLTYPE SetPushSourceFlags(ULONG Flags) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetStreamOffset(REFERENCE_TIME rtOffset) override { return NOERROR; }
	HRESULT STDMETHODCALLTYPE GetStreamOffset(REFERENCE_TIME *prtOffset) override { *prtOffset = 0; return NOERROR; }
	HRESULT STDMETHODCALLTYPE GetMaxStreamOffset(REFERENCE_TIME *prtMaxOffset) override { *prtMaxOffset = 0; return NOERROR; }
	HRESULT STDMETHODCALLTYPE SetMaxStreamOffset(REFERENCE_TIME rtMaxOffset) override { return NOERROR; }
};

class CCaptureProperties : public CBasePropertyPage
{
public:
	static CUnknown * WINAPI CreateInstance(LPUNKNOWN lpunk, HRESULT *phr)
	{
		CUnknown *punk = new CCaptureProperties(lpunk, phr);
		*phr = (punk ? S_OK : E_OUTOFMEMORY);
		return punk;
	}

private:
	CCaptureProperties(LPUNKNOWN lpunk, HRESULT *phr) : CBasePropertyPage("", lpunk, -1, -1) { }

	STDMETHODIMP Activate(HWND hwndParent, LPCRECT prect, BOOL fModal) 
	{
		struct MyData
		{
			#pragma pack(4)
			DLGTEMPLATE Header;
			#pragma pack(2)
			WORD NoMenu, StdClass; wchar_t Title[1]; // 0 - no menu | 0 - standard dialog class | No title
			#pragma pack(4)
			struct Item
			{
				#pragma pack(4)
				DLGITEMTEMPLATE Header;
				#pragma pack(2)
				WORD FFFF, ClassID; wchar_t Text[2]; WORD NoData;
				#pragma pack(4)
			} Items[8];
			#pragma pack(4)
		} md = {
			{ WS_CHILD | WS_VISIBLE | DS_CENTER, NULL, sizeof(md.Items)/sizeof(MyData::Item) }, 0, 0, L"", {
			{ { WS_VISIBLE | WS_CHILD | SS_LEFT,                       NULL ,  5, 18,   80,  10, 1000 }, 0xFFFF, 0x0082, L"-" }, //Label
			{ { WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST, NULL , 90, 17,  150, 100, 1001 }, 0xFFFF, 0x0085, L"-" }, //Combo Box
			{ { WS_VISIBLE | WS_CHILD | SS_LEFT,                       NULL ,  5, 36,   80,  10, 1002 }, 0xFFFF, 0x0082, L"-" }, //Label
			{ { WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST, NULL , 90, 35,  150, 100, 1003 }, 0xFFFF, 0x0085, L"-" }, //Combo Box
			{ { WS_VISIBLE | WS_CHILD | SS_LEFT,                       NULL ,  5, 54,   80,  10, 1004 }, 0xFFFF, 0x0082, L"-" }, //Label
			{ { WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST, NULL , 90, 53,  150, 100, 1005 }, 0xFFFF, 0x0085, L"-" }, //Combo Box
			{ { WS_VISIBLE | WS_CHILD | SS_LEFT,                       NULL ,  5, 72,   80,  10, 1006 }, 0xFFFF, 0x0082, L"-" }, //Label
			{ { WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_CHECKBOX,      NULL , 90, 71,  150,  10, 1007 }, 0xFFFF, 0x0080, L"-" }, //Check Box
		}};

		HWND hwnd = CreateDialogIndirectParamW(NULL, &md.Header, hwndParent, &MyDialogProc, (LPARAM)this);
		SetDlgItemTextW(hwnd, 1000, L"Resolution mismatch:");
		SetDlgItemTextW(hwnd, 1002, L"Unity never started:");
		SetDlgItemTextW(hwnd, 1004, L"Unity sending stopped:");
		SetDlgItemTextW(hwnd, 1006, L"Display FPS:");
		SetDlgItemTextW(hwnd, 1007, L"Show capture frame rate");
		for (int i = 0; i < 3; i++)
		{
			HWND hWndComboBox = GetDlgItem(hwnd, 1001 + i*2);
			for (int j = 0; j < sizeof(ErrorDrawModeNames)/sizeof(ErrorDrawModeNames[0]); j++)
				SendMessageW(hWndComboBox, (UINT)CB_ADDSTRING, (WPARAM)0, (LPARAM)ErrorDrawModeNames[j]);
			SendMessageA(hWndComboBox, CB_SETCURSEL, (WPARAM)ErrorDrawModes[i], (LPARAM)0);
		}
		SendMessage(GetDlgItem(hwnd, 1007), BM_SETCHECK, (OutputFrameRate ? BST_CHECKED : BST_UNCHECKED), 0);

		SetWindowPos(hwnd, NULL, prect->left, prect->top, prect->right-prect->left, prect->bottom-prect->top, 0); //show in tab page
		return S_OK;
	}

	static INT_PTR CALLBACK MyDialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		if (uMsg == WM_INITDIALOG) return TRUE;
		if (uMsg == WM_COMMAND)
		{
			//DebugLog("[DIALOG] WM_COMMAND - ItemID: %d - SubCommand: %d - Value: %d\n", (int)LOWORD(wParam), (int)HIWORD(wParam), (int)lParam);
			int ItemID = LOWORD(wParam), SubCommand = HIWORD(wParam);
			HWND hWndItem = GetDlgItem(hwnd, ItemID);
			int SelectionIndex = (int)SendMessageA(hWndItem, CB_GETCURSEL, 0, 0);
			if (ItemID == 1001 && SubCommand == 1) ErrorDrawModes[EDC_ResolutionMismatch]  = (EErrorDrawMode)SelectionIndex;
			if (ItemID == 1003 && SubCommand == 1) ErrorDrawModes[EDC_UnityNeverStarted]   = (EErrorDrawMode)SelectionIndex;
			if (ItemID == 1005 && SubCommand == 1) ErrorDrawModes[EDC_UnitySendingStopped] = (EErrorDrawMode)SelectionIndex;
			if (ItemID == 1007) SendMessage(hWndItem, BM_SETCHECK, ((OutputFrameRate ^= 1) ? BST_CHECKED : BST_UNCHECKED), 0);
			return TRUE;
		}
		return FALSE;
	}

	STDMETHODIMP GetPageInfo(__out LPPROPPAGEINFO pPageInfo)
	{
		pPageInfo->pszTitle = (WCHAR*)CoTaskMemAlloc(sizeof(CaptureSourceName));
		memcpy(pPageInfo->pszTitle, CaptureSourceName, sizeof(CaptureSourceName));
		pPageInfo->size.cx      = 490;
		pPageInfo->size.cy      = 200;
		pPageInfo->pszDocString = NULL;
		pPageInfo->pszHelpFile  = NULL;
		pPageInfo->dwHelpContext= 0;
		return NOERROR;
	}
};

class CCaptureSource : CSource, IQualityControl, ICamSource, ISpecifyPropertyPages
{
public:
	static CUnknown * WINAPI CreateInstance(LPUNKNOWN lpunk, HRESULT *phr)
	{
		ASSERT(phr);
		*phr = S_OK;

		CCaptureSource *pSource = new CCaptureSource(lpunk, phr);
		if (FAILED(*phr) || !pSource)
		{
			if (!pSource) *phr = E_OUTOFMEMORY;
			delete pSource;
			return NULL;
		}

		CCaptureStream* pStream = new CCaptureStream(pSource, phr);
		if (FAILED(*phr) || !pStream)
		{
			if (!pStream) *phr = E_OUTOFMEMORY;
			delete pStream;
			delete pSource;
			return NULL;
		}

		return pSource;
	}

private:
	DECLARE_IUNKNOWN;

	CCaptureSource(LPUNKNOWN lpunk, HRESULT* phr) : CSource("Source", lpunk, CLSID_UnityCaptureService, phr) { }

	//CSource
	STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void ** ppv) override
	{
		if (ppv == NULL) return E_POINTER;
		if      (riid == IID_IQualityControl      ) { *ppv = (IQualityControl*)this;       AddRef(); return S_OK; }
		else if (riid == IID_ICamSource           ) { *ppv = (ICamSource*)this;            AddRef(); return S_OK; }
		else if (riid == IID_ISpecifyPropertyPages) { *ppv = (ISpecifyPropertyPages*)this; AddRef(); return S_OK; } //
		return CSource::NonDelegatingQueryInterface(riid, ppv);
	}

	//IQualityControl
	STDMETHODIMP Notify(IBaseFilter *pSelf, Quality q) override { return S_OK; }
	STDMETHODIMP SetSink(IQualityControl *piqc) override { return S_OK; }

	//ISpecifyPropertyPages
	STDMETHODIMP GetPages(CAUUID * pPages) override
	{
		CheckPointer(pPages,E_POINTER);
		pPages->cElems = 1;
		pPages->pElems = (GUID *) CoTaskMemAlloc(sizeof(GUID));
		if (pPages->pElems == NULL) return E_OUTOFMEMORY;
		*(pPages->pElems) = CLSID_UnityCaptureProperties;
		return NOERROR;
	}
};

static const AMOVIESETUP_MEDIATYPE sudMediaTypesCaptureSourceOut = { &MEDIATYPE_Video, &MEDIASUBTYPE_NULL };
static const AMOVIESETUP_PIN sudCaptureSourceOut = {
	L"Output",   // Pin string name
	FALSE,       // Is it rendered
	TRUE,        // Is it an output
	FALSE,       // Can we have none
	FALSE,       // Can we have many
	&CLSID_NULL, // Connects to filter
	NULL,        // Connects to pin
	1,           // Number of types
	&sudMediaTypesCaptureSourceOut // Pin Media types
};
static const AMOVIESETUP_FILTER sudCaptureSource = {
	&CLSID_UnityCaptureService, // Filter CLSID
	CaptureSourceName,          // String name
	MERIT_DO_NOT_USE,           // Filter merit
	1,                          // Number pins
	&sudCaptureSourceOut        // Pin details
};

//Global template instance (used extern by strmbase.lib/strmbasd.lib)
CFactoryTemplate g_Templates[] = { 
	{ CaptureSourceName,                    &CLSID_UnityCaptureService,    CCaptureSource::CreateInstance,     NULL, &sudCaptureSource },
	{ CaptureSourceName L" Configuration" , &CLSID_UnityCaptureProperties, CCaptureProperties::CreateInstance, NULL, NULL },
};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

// Stack Overflow - "Fake" DirectShow video capture device
// http://stackoverflow.com/questions/1376734/fake-directshow-video-capture-device
STDAPI AMovieSetupRegisterServer(CLSID   clsServer, LPCWSTR szDescription, LPCWSTR szFileName, LPCWSTR szThreadingModel = L"Both", LPCWSTR szServerType = L"InprocServer32");
STDAPI AMovieSetupUnregisterServer(CLSID clsServer);
static HRESULT RegisterFilters(BOOL bRegister)
{
	HRESULT hr = NOERROR;
	WCHAR achFileName[MAX_PATH];
	char achTemp[MAX_PATH];
	ASSERT(g_hInst != 0);

	if (0 == GetModuleFileNameA(g_hInst, achTemp, sizeof(achTemp))) return AmHresultFromWin32(GetLastError());
	MultiByteToWideChar(CP_ACP, 0L, achTemp, lstrlenA(achTemp) + 1, achFileName, (sizeof(achFileName)/sizeof((achFileName)[0])));

	hr = CoInitialize(0);
	if (bRegister)
	{
		hr = AMovieSetupRegisterServer(CLSID_UnityCaptureService, CaptureSourceName, achFileName, L"Both", L"InprocServer32");
		if (FAILED(hr)) MessageBoxA(0, "Service AMovieSetupRegisterServer failed", "RegisterFilters setup", NULL);
		hr = AMovieSetupRegisterServer(CLSID_UnityCaptureProperties, CaptureSourceName L" Configuration", achFileName, L"Both", L"InprocServer32");
		if (FAILED(hr)) MessageBoxA(0, "Properties AMovieSetupRegisterServer failed", "RegisterFilters setup", NULL);
	}

	if (SUCCEEDED(hr))
	{
		IFilterMapper2 *fm = NULL;
		hr = CoCreateInstance(CLSID_FilterMapper2, NULL, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void **)&fm);

		if (SUCCEEDED(hr))
		{
			if (bRegister)
			{
				IMoniker *pMoniker = 0;
				REGFILTER2 rf2;
				rf2.dwVersion = 1;
				rf2.dwMerit = MERIT_DO_NOT_USE;
				rf2.cPins = 1;
				rf2.rgPins = &sudCaptureSourceOut;
				hr = fm->RegisterFilter(CLSID_UnityCaptureService, CaptureSourceName, 0, &CLSID_VideoInputDeviceCategory, NULL, &rf2);
				if (FAILED(hr)) MessageBoxA(0, "Service RegisterFilter of IFilterMapper2 failed", "RegisterFilters setup", NULL);
			}
			else
			{
				hr = fm->UnregisterFilter(&CLSID_VideoInputDeviceCategory, 0, CLSID_UnityCaptureService);
				if (FAILED(hr)) MessageBoxA(0, "Service UnregisterFilter of IFilterMapper2 failed", "RegisterFilters setup", NULL);
			}
		}
		if (fm)
		{
			fm->Release();
			fm = NULL;
		}
	}

	if (SUCCEEDED(hr) && !bRegister)
	{
		hr = AMovieSetupUnregisterServer(CLSID_UnityCaptureService);
		if (FAILED(hr)) MessageBoxA(0, "Service AMovieSetupUnregisterServer failed", "RegisterFilters setup", NULL);
		hr = AMovieSetupUnregisterServer(CLSID_UnityCaptureProperties);
		if (FAILED(hr)) MessageBoxA(0, "Properties AMovieSetupUnregisterServer failed", "RegisterFilters setup", NULL);
	}

	CoFreeUnusedLibraries();
	CoUninitialize();
	return hr;
}

STDAPI DllRegisterServer()
{
	HRESULT res = RegisterFilters(TRUE);

	//This lets Unity and Skype access the virtual camera too!
	//Thanks to: https://social.msdn.microsoft.com/Forums/windowsdesktop/en-US/cd2b9d2d-b961-442d-8946-fdc038fed530/where-to-specify-device-id-in-the-filter?forum=windowsdirectshowdevelopment
	LPOLESTR olestr_CLSID;
	StringFromCLSID(CLSID_UnityCaptureService, &olestr_CLSID);
	std::wstring wstr_CLSID(olestr_CLSID);

	LPOLESTR guidString;
	StringFromCLSID(CLSID_VideoInputDeviceCategory, &guidString);
	std::wstring inputCat = guidString;
	std::wstring str_video_capture_device_key = L"CLSID\\" + inputCat + L"\\Instance";
	//std::string str_video_capture_device_key("SOFTWARE\\Classes\\CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\");
	str_video_capture_device_key.append(wstr_CLSID);

	HKEY hKey;
	RegOpenKeyExW(HKEY_LOCAL_MACHINE, str_video_capture_device_key.c_str(), 0, KEY_ALL_ACCESS, &hKey);
	LPCSTR value = ("DevicePath");
	LPCSTR data = "foo:bar";
	RegSetValueExA(hKey, value, 0, REG_SZ, (LPBYTE)data, (DWORD)strlen(data) + 1);
	RegCloseKey(hKey);
	return res;
}

STDAPI DllUnregisterServer()
{
	return RegisterFilters(FALSE);
}

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);
BOOL APIENTRY DllMain( HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	return DllEntryPoint((HINSTANCE)(hModule), ul_reason_for_call, lpReserved);
}
