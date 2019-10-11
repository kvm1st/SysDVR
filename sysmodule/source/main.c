#include <stdlib.h>
#include <switch.h>
#include <pthread.h>

#include "grcd.h"

//#define MODE_USB
#define MODE_SOCKET

#if defined(MODE_USB) && defined(MODE_SOCKET)
#error Define only one between MODE_USB and MODE_SOCKET
#elif !defined(MODE_USB) && !defined(MODE_SOCKET)
#pragma message "No mode has been defined, dafaulting to MODE_USB"
#define MODE_USB
#endif

#if !defined(__SWITCH__)
//Silence visual studio errors
#define __attribute__(x) 
typedef u64 ssize_t;
#endif

extern u32 __start__;
u32 __nx_applet_type = AppletType_None;
#if defined(MODE_USB)
	#define INNER_HEAP_SIZE 0x80000
#else
	#define INNER_HEAP_SIZE 0x300000
#endif
size_t nx_inner_heap_size = INNER_HEAP_SIZE;
char nx_inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void)
{
	void*  addr = nx_inner_heap;
	size_t size = nx_inner_heap_size;

	// Newlib
	extern char* fake_heap_start;
	extern char* fake_heap_end;

	fake_heap_start = (char*)addr;
	fake_heap_end   = (char*)addr + size;
}

void __attribute__((weak)) __appInit(void)
{
	svcSleepThread(2E+10);

	Result rc;

	rc = smInitialize();
	if (R_FAILED(rc))
		fatalSimple(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

	rc = fsInitialize();
	if (R_FAILED(rc))
		fatalSimple(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

	rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
	
	if (R_FAILED(rc))
		fatalSimple(MAKERESULT(1, 10));
	
	fsdevMountSdmc();
}

void __attribute__((weak)) __appExit(void)
{
	fsdevUnmountAll();
	fsExit();
#if defined(MODE_USB)
	usbCommsExit();
#else
	socketExit();
#endif
	smExit();
}

const int VbufSz = 0x32000;
const int AbufSz = 0x1000;
const int AudioBatchSz = 12;

u8* Vbuf = NULL;
u8* Abuf = NULL;
u32 VOutSz = 0;
u32 AOutSz = 0;

Service grcdVideo;
Service grcdAudio;

void AllocateRecordingBuf() 
{
	Vbuf = aligned_alloc(0x1000, VbufSz);
	if (!Vbuf)
		fatalSimple(MAKERESULT(1, 40));

	Abuf = aligned_alloc(0x1000, AbufSz * AudioBatchSz);
	if (!Vbuf)
		fatalSimple(MAKERESULT(1, 50));
}

void FreeRecordingBuf()
{
	free(Vbuf);
	free(Abuf);
}

Result OpenGrcdForThread(GrcStream stream) 
{
	Result rc;
	if (stream == GrcStream_Audio)
		rc = grcdServiceOpen(&grcdAudio);
	else 
	{
		rc = grcdServiceOpen(&grcdVideo);
		if (R_FAILED(rc)) return rc;
		rc = grcdServiceBegin(&grcdVideo);
	}
	return rc;
}

//Batch sending audio samples to improve speed
static void ReadAudioStream()
{
	u32 unk = 0;
	u64 timestamp = 0;
	u32 TmpAudioSz = 0;
	AOutSz = 0;
	for (int i = 0; i < AudioBatchSz; i++)
	{
		Result res = grcdServiceRead(&grcdAudio, GrcStream_Audio, Abuf + AOutSz, AbufSz, &unk, &TmpAudioSz, &timestamp);
		if (R_FAILED(res) || TmpAudioSz <= 0)
		{
			--i;
			continue;
		}
		AOutSz += TmpAudioSz;
	}
}

static void ReadVideoStream()
{
	u32 unk = 0;
	u64 timestamp = 0;

	while (true) {
		Result res = grcdServiceRead(&grcdVideo, GrcStream_Video, Vbuf, VbufSz, &unk, &VOutSz, &timestamp);
		if (R_FAILED(res) || VOutSz <= 0)
		{
			VOutSz = 0;
			svcSleepThread(5000000);
			continue;
		}
		break;
	}
}

#if defined(MODE_USB)
const u32 VideoStream = 0;
const u32 AudioStream = 1;
const u32 REQMAGIC_VID = 0xAAAAAAAA;
const u32 REQMAGIC_AUD = 0xBBBBBBBB;

static u32 WaitForInputReq(u32 dev)
{
	while (true)
	{
		u32 initSeq = 0;
		if (usbCommsReadEx(&initSeq, sizeof(initSeq), dev) == sizeof(initSeq))
			return initSeq;
	}
	return 0;
}

static void SendStream(GrcStream stream, u32 Dev)
{
	u32* size = stream == GrcStream_Video ? &VOutSz : &AOutSz;
	 
	if (*size <= 0)
	{
		*size = 0;
		usbCommsWriteEx(size, sizeof(*size), Dev);
	}

	u8* TargetBuf = stream == GrcStream_Video ? Vbuf : Abuf;

	if (usbCommsWriteEx(size, sizeof(*size), Dev) != sizeof(*size)) return;
	if (usbCommsWriteEx(TargetBuf, *size, Dev) != *size) return;
	return;
}

void* StreamThreadMain(void* _stream)
{
	GrcStream stream = (GrcStream)_stream;
	const u32 Dev = stream == GrcStream_Video ? VideoStream : AudioStream;
	u8 ErrorCode = stream == GrcStream_Video ? 70 : 80;
	u32 ThreadMagic = stream == GrcStream_Video ? REQMAGIC_VID : REQMAGIC_AUD;

	void (*ReadStreamFn)() = stream == GrcStream_Video ? ReadVideoStream : ReadAudioStream;

	{
		Result rc = OpenGrcdForThread(stream);
		if (R_FAILED(rc)) fatalSimple(rc);
	}

	while (true)
	{
		u32 cmd = WaitForInputReq(Dev);

		if (cmd == ThreadMagic)
		{
			ReadStreamFn();
			SendStream(stream, Dev);
		}
		else fatalSimple(MAKERESULT(1, ErrorCode));
	}
	return NULL;
}
#else
#ifdef __SWITCH__
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#else
//not actually used, just to stop visual studio from complaining.
//~~i regret nothing~~
#define F_SETFL 1
#define O_NONBLOCK 1
#define F_GETFL 1
#include <WinSock2.h>
#endif

Result CreateSocket(int *OutSock, int port, int baseError)
{
	int err = 0, sock = -1;
	struct sockaddr_in temp;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return MAKERESULT(baseError, 1);
	
	temp.sin_family = AF_INET;
	temp.sin_addr.s_addr = INADDR_ANY;
	temp.sin_port = htons(port);

	//We don't actually want a non-blocking socket but this is a workaround for the issue described in StreamThreadMain
	fcntl(sock, F_SETFL, O_NONBLOCK);

	const int optVal = 1;
	err = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*)&optVal, sizeof(optVal));
	if (err)
		return MAKERESULT(baseError, 2);

	err = bind(sock, (struct sockaddr*) & temp, sizeof(temp));
	if (err)
		return MAKERESULT(baseError, 3);

	err = listen(sock, 1);
	if (err)
		return MAKERESULT(baseError, 4);

	*OutSock = sock;
	return 0;
}

int VideoSock = -1;
int AudioSock = -1;
int AudioCurSock = -1;
int VideoCurSock = -1;

Result SocketInit(GrcStream stream)
{
	Result rc;
	if (stream == GrcStream_Video)
	{
		rc = CreateSocket(&VideoSock, 6666, 2);
		if (R_FAILED(rc)) return rc;
	}
	else 
	{
		rc = CreateSocket(&AudioSock, 6667, 3);
		if (R_FAILED(rc)) return rc;
	}
	return 0;
}

const u8 SPS[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B, 0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80 };
const u8 PPS[] = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };

void* StreamThreadMain(void* _stream)
{
	GrcStream stream = (GrcStream)_stream;
	void (*ReadStreamFn)() = stream == GrcStream_Video ? ReadVideoStream : ReadAudioStream;

	u32* size = stream == GrcStream_Video ? &VOutSz : &AOutSz;
	u8* TargetBuf = stream == GrcStream_Video ? Vbuf : Abuf;

	int* sock = stream == GrcStream_Video ? &VideoSock : &AudioSock;
	int* OutSock = stream == GrcStream_Video ? &VideoCurSock : &AudioCurSock;

	{
		Result rc = OpenGrcdForThread(stream);
		if (R_FAILED(rc)) fatalSimple(rc);
		rc = SocketInit(stream);
		if (R_FAILED(rc)) fatalSimple(rc);
	}

	while (true) {
		int curSock = accept(*sock, 0, 0);
		if (curSock < 0)
		{
			svcSleepThread(3E+9);
			continue;
		}
		
		/*
			There appear to be an issue with socketing, even if the video and audio listeners are used on different threads
			for some reason calling accept on one of them blocks the other thread as well, even while a client is connected.
			The workaround is making the socket non-blocking and then to set the client socket as blocking.
			By default the socket returned from accept inherits this flag.
		*/
		fcntl(curSock, F_SETFL, fcntl(curSock, F_GETFL, 0) & ~O_NONBLOCK);

		*OutSock = curSock;

		int res = 0;

		if (stream == GrcStream_Video) {
			res = write(curSock, SPS, sizeof(SPS));
			if (res == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) fatalSimple(MAKERESULT(66,66));
			res = write(curSock, PPS, sizeof(PPS));
			if (res == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) fatalSimple(MAKERESULT(66, 66));
		}

		while (true)
		{
			ReadStreamFn();
			res = write(curSock, TargetBuf, *size);
			if (res == -1)
			{
				if (errno == EWOULDBLOCK || errno == EAGAIN) fatalSimple(MAKERESULT(66, 66));
				else break;
			}
		}

		close(curSock);
		*OutSock = -1;
		svcSleepThread(1E+9);
	}
	return NULL;
}
#endif


int main(int argc, char* argv[])
{
#if defined(MODE_USB)
	if (R_FAILED(usbCommsInitializeEx(2, NULL)))
		fatalSimple(MAKERESULT(1, 60));
#else
	Result rc = socketInitializeDefault();
	if (R_FAILED(rc)) fatalSimple(rc);
#endif

	AllocateRecordingBuf();

	pthread_t audioThread;
	if (pthread_create(&audioThread, NULL, StreamThreadMain, (void*)GrcStream_Audio))
		fatalSimple(MAKERESULT(1, 90));

	StreamThreadMain((void*)GrcStream_Video);

	pthread_join(audioThread, NULL);

	grcdServiceClose(&grcdVideo);
	grcdServiceClose(&grcdAudio);
	FreeRecordingBuf();

    return 0;
}