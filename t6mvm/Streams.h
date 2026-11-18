#pragma once
#define _CRT_SECURE_NO_WARNINGS 
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include "IMVMStream.h"
#include "Default.h"
#include "GreenScreen.h"
#include "NoPlayers.h"
#include "NoGun.h"
#include "Depth.h"
#include "FxOnly.h"
#include "DepthWithGun.h"
#include "GreenSky.h"
#include <filesystem>
#include "LocalAddresses.h"
#include "ScreenGrab.h"
#include "FFmpegEncoder.h"
#include "JpegCapture.h"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <utility>
#include <cassert>
#include "..\MinHook\MinHook.h"
#pragma comment(lib, "libMinHook.x86.lib")
#include <sstream>
#include <libgmavi.h>
#include "Settings.h"
#include "kiero.h"
#include "WinToastHelper.h"
#include <codecvt>


#pragma comment(lib, "d3d11.lib")

namespace Streams
{
	// Frames are copied to CPU memory on the render thread, then encoded and
	// written by a worker. This overlaps rendering with WIC compression / disk I/O
	// and keeps the tick hook from blocking on them.
	enum class AsyncFrameKind { Image, Avi };

	struct AsyncFrameJob
	{
		std::vector<uint8_t> Pixels{};
		UINT Width = 0;
		UINT Height = 0;
		GUID Container{};
		std::wstring Path{};
		void* Gmav = nullptr;
		AsyncFrameKind Kind = AsyncFrameKind::Image;
	};

	inline static std::deque<AsyncFrameJob> AsyncQueue{};
	inline static std::mutex AsyncQueueMutex{};
	inline static std::condition_variable AsyncQueueReady{};
	inline static std::condition_variable AsyncQueueSpace{};
	inline static HANDLE AsyncWriterThread = nullptr;
	inline static bool AsyncWriterStop = false;
	inline static int AsyncEncodeFailures = 0;
	static constexpr size_t MaxQueuedAsyncFrames = 16;

	static void EnqueueAsyncFrame(AsyncFrameJob&& job)
	{
		// Backpressure: the tick hook must not outrun the writer, otherwise the
		// demo advances past frames we never encoded and the output is truncated.
		unique_lock<mutex> lock(AsyncQueueMutex);
		AsyncQueueSpace.wait(lock, []() { return AsyncQueue.size() < MaxQueuedAsyncFrames || AsyncWriterStop; });
		if (AsyncWriterStop)
			return;
		AsyncQueue.push_back(std::move(job));
		AsyncQueueReady.notify_one();
	}

	static DWORD WINAPI AsyncFrameWriterThread(LPVOID)
	{
		// WIC COM objects require COM on the thread that creates them.
		CoInitialize(nullptr);
		static std::vector<BYTE> aviRgbBuffer{};
		for (;;)
		{
			AsyncFrameJob job{};
			{
				unique_lock<mutex> lock(AsyncQueueMutex);
				AsyncQueueReady.wait(lock, []() { return !AsyncQueue.empty() || AsyncWriterStop; });
				if (AsyncQueue.empty())
				{
					if (AsyncWriterStop)
						break;
					continue;
				}
				job = std::move(AsyncQueue.front());
				AsyncQueue.pop_front();
			}
			AsyncQueueSpace.notify_one();

			if (job.Kind == AsyncFrameKind::Image)
			{
				if (FAILED(WicImage::EncodeToFile(job.Pixels.data(), job.Width, job.Height,
					job.Container, job.Path.c_str())))
				{
					AsyncEncodeFailures++;
				}
			}
			else
			{
				const size_t frameSize = static_cast<size_t>(job.Width) * job.Height * 3;
				aviRgbBuffer.resize(frameSize);
				BYTE* dstBase = aviRgbBuffer.data();
				const BYTE* srcBase = job.Pixels.data();
				const size_t srcPitch = static_cast<size_t>(job.Width) * 4;
				const size_t dstPitch = static_cast<size_t>(job.Width) * 3;
				for (UINT y = 0; y < job.Height; ++y)
				{
					const BYTE* src = srcBase + static_cast<size_t>(job.Height - 1 - y) * srcPitch;
					BYTE* dst = dstBase + static_cast<size_t>(y) * dstPitch;
					for (UINT x = 0; x < job.Width; ++x, src += 4, dst += 3)
					{
						dst[0] = src[2];
						dst[1] = src[1];
						dst[2] = src[0];
					}
				}
				if (!gmav_add(job.Gmav, aviRgbBuffer.data()) && job.Gmav)
					gmav_finish(job.Gmav);
			}
		}
		CoUninitialize();
		return 0;
	}

	static bool StartAsyncFrameWriter()
	{
		{
			lock_guard<mutex> lock(AsyncQueueMutex);
			AsyncQueue.clear();
			AsyncWriterStop = false;
		}
		AsyncEncodeFailures = 0;
		AsyncWriterThread = CreateThread(nullptr, 0, &AsyncFrameWriterThread, nullptr, 0, nullptr);
		if (!AsyncWriterThread)
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Could not create the async frame writer thread.");
			return false;
		}
		return true;
	}

	static void StopAsyncFrameWriter(bool discard)
	{
		if (!AsyncWriterThread)
			return;
		{
			lock_guard<mutex> lock(AsyncQueueMutex);
			if (discard)
				AsyncQueue.clear();
			AsyncWriterStop = true;
		}
		AsyncQueueReady.notify_all();
		AsyncQueueSpace.notify_all();
		WaitForSingleObject(AsyncWriterThread, INFINITE);
		CloseHandle(AsyncWriterThread);
		AsyncWriterThread = nullptr;
		if (AsyncEncodeFailures > 0)
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to encode %i queued image(s).", AsyncEncodeFailures);
	}

	void* gmav = 0x00;
#pragma region AviFile methods
	// Capture the back buffer to CPU memory and hand it to the writer thread.
	static bool CaptureBackBufferAndQueueAvi(ID3D11DeviceContext* context, ID3D11Texture2D* backBuffer)
	{
		D3D11_TEXTURE2D_DESC desc{};
		backBuffer->GetDesc(&desc);

		std::vector<uint8_t> pixels{};
		if (FAILED(RawCapture::CaptureBGRA(context, backBuffer, pixels)) || pixels.empty())
			return false;

		AsyncFrameJob job{};
		job.Kind = AsyncFrameKind::Avi;
		job.Pixels = std::move(pixels);
		job.Width = desc.Width;
		job.Height = desc.Height;
		job.Gmav = gmav;
		EnqueueAsyncFrame(std::move(job));
		return true;
	}
#pragma endregion
#pragma region Streams defs
	int frameCount = 0;
	inline static bool IsStreamsStarted = false;
	inline static bool ScreenshotRequested = false;
	inline static bool IsAnyOtherStream = false;
	inline static bool StreamShouldIgnoreReshade = false;
	bool FreeBufferRequested = false;
	string filename = "";
	inline static bool IsStreamsRunning = false;
	inline static bool RecordingAsFFmpeg = false;
	inline static bool RecordingAsAvi = false;
	inline static bool IsFFmpegEncoding = false;
	inline static bool FFmpegEncodeFailed = false;
	static const char* newStreamFolder = "";
	static string folderToDeleteWhenAborted = "";
	inline static vector<IMVMStream*> FFmpegStreams{};

	// One live FFmpeg process per enabled pass. Captured frames are queued and
	// written to the process stdin by a worker thread so the Present path never
	// waits on the encoder.
	struct FFmpegPipeTarget
	{
		FFmpeg::PipeEncoder Encoder{};
		deque<vector<uint8_t>> Queue{};
		mutex QueueMutex{};
		condition_variable QueueReady{};
		condition_variable QueueSpace{};
		HANDLE Thread = nullptr;
		bool StopRequested = false;
		bool WriteFailed = false;
		int EncodeFailures = 0;
	};
	static constexpr size_t MaxQueuedFrames = 16;
	inline static vector<unique_ptr<FFmpegPipeTarget>> PipeTargets{};
	inline static FFmpegPipeTarget* CurrentPipeTarget = nullptr;

	inline static bool IsFFmpegMode()
	{
		return CustomDvars::dvar_streams_ffmpeg && CustomDvars::dvar_streams_ffmpeg->current.enabled;
	}

	inline static bool IsAviMode()
	{
		return CustomDvars::dvar_streams_avi && CustomDvars::dvar_streams_avi->current.enabled && !IsFFmpegMode();
	}

	inline static vector<IMVMStream*> Streams{};
	inline static Stream_Default Default;
	inline static Stream_GreenScreen GreenScreen;
	inline static Stream_NoPlayers NoPlayers;
	inline static Stream_NoGun NoGun;
	inline static Stream_Depth Depth;
	inline static Stream_DepthWithGun DepthWithGun;
	inline static Stream_FxOnly FxOnly;
	inline static Stream_GreenSky GreenSky;

	cmd_function_s cmd_streams_start_VAR{};
	cmd_function_s cmd_streams_stop_VAR{};
	cmd_function_s cmd_streams_abort_VAR{};

#pragma endregion
#pragma region DX11 defs
	typedef long(__stdcall* present)(IDXGISwapChain*, UINT, UINT);
	present p_present;
	present p_present_target;

	
	typedef long(__stdcall* resizeBuffers)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
	resizeBuffers p_ResizeBuffers;
	resizeBuffers p_ResizeBuffers_target;

	typedef long(__stdcall* resizeTarget)(IDXGISwapChain*, const DXGI_MODE_DESC*);
	resizeTarget p_ResizeTarget;
	resizeTarget p_ResizeTarget_target;

	HWND window = NULL;
	ID3D11Device* p_device = NULL;
	ID3D11DeviceContext* p_context = NULL;
	ID3D11RenderTargetView* mainRenderTargetView = NULL;
	DXGI_SWAP_CHAIN_DESC sd;


	ID3D11Texture2D* texture_to_save = NULL;

	// The capture texture is reused between frames; drop it when the swap chain
	// changes size so the next capture recreates it with the new dimensions.
	static void ReleaseCaptureTexture()
	{
		if (texture_to_save)
		{
			texture_to_save->Release();
			texture_to_save = NULL;
		}
	}

#pragma endregion
#pragma region DX11 methods
	bool get_present_pointer()
	{
		DXGI_SWAP_CHAIN_DESC sd;
		ZeroMemory(&sd, sizeof(sd));
		sd.BufferCount = 2;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = GetForegroundWindow();
		sd.SampleDesc.Count = 1;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	
		IDXGISwapChain* swap_chain;
		ID3D11Device* device;
	
		const D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
		if (D3D11CreateDeviceAndSwapChain(
			NULL,
			D3D_DRIVER_TYPE_HARDWARE,
			NULL,
			0,
			feature_levels,
			2,
			D3D11_SDK_VERSION,
			&sd,
			&swap_chain,
			&device,
			nullptr,
			nullptr) == S_OK)
		{
			void** p_vtable = *reinterpret_cast<void***>(swap_chain);
			swap_chain->Release();
			device->Release();
			//context->Release();
			p_present_target = (present)p_vtable[8];
			p_ResizeBuffers_target = (resizeBuffers)p_vtable[13];
			p_ResizeTarget_target = (resizeTarget)p_vtable[14];
			return true;
		}
		return false;
	}

	static void ShowProgressOnScreen()
	{
		if(!T6SDK::Dvars::GetBool(CustomDvars::dvar_streams))
			return;
		if(!IsStreamsRunning)
			return;
		if (IsFFmpegEncoding)
			return;
		if (T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickStart) > -1 && T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickEnd) > -1)
		{
			int totalFrames = (T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickEnd) - T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickStart))/(1000 / T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_fps));
			float percent = (float)frameCount / (float)totalFrames;

			char buffer0[256];
			sprintf(buffer0, "Progress: ^3%i/%i ^7(%.0f%%)", frameCount, totalFrames, percent * 100.0f);
			vec2_t coords = T6SDK::Drawing::GetGridCellCoords(8, 19);
			T6SDK::Drawing::DrawTextAbsolute(buffer0, coords.x, coords.y, 1.5f, T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::Center, 0x00);

			char formatBuffer1[96];
			sprintf(formatBuffer1, "%s", RecordingAsFFmpeg ? "Recording as ^3FFMPEG ^7(piped frames)" : RecordingAsAvi ? "Recording as ^3AVI" : T6SDK::Dvars::GetBool(CustomDvars::dvar_streams_JPG) ? "Recording as ^3JPG ^7sequence" : "Recording as ^3PNG ^7sequence");
			
			vec2_t coords2 = T6SDK::Drawing::GetGridCellCoords(8, 21);
			T6SDK::Drawing::DrawTextAbsolute(formatBuffer1, coords2.x, coords2.y, 1.0f, T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::Center, 0x00);

			vec2_t coords3 = T6SDK::Drawing::GetGridCellCoords(8, 22);

			char buffer2[256];
			sprintf(buffer2, "Press ^3%s ^7to stop. Press ^1%s ^7to abort recording and remove recorded files.", T6SDK::Input::GetKeyByCode(Settings::Settings::KeyBinds["StreamsStartStop"])->KeyName.c_str(), T6SDK::Input::GetKeyByCode(Settings::Settings::KeyBinds["StreamsAbort"])->KeyName.c_str());
			T6SDK::Drawing::DrawTextAbsolute(buffer2, coords3.x, coords3.y, 1.0f, T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::Center, 0x00);
		}
		else
		{
			char buffer1[256];
			sprintf(buffer1, "Captured ^3%i ^7frames", frameCount);
			vec2_t coords = T6SDK::Drawing::GetGridCellCoords(8, 19);
			T6SDK::Drawing::DrawTextAbsolute(buffer1, coords.x, coords.y, 1.5f, T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::Center, 0x00);

			vec2_t coords2 = T6SDK::Drawing::GetGridCellCoords(8, 21);
			char buffer3[256];
			sprintf(buffer3, "Press ^3%s ^7to stop. Press ^1%s ^7to abort recording and remove recorded files.", T6SDK::Input::GetKeyByCode(Settings::Settings::KeyBinds["StreamsStartStop"])->KeyName.c_str(), T6SDK::Input::GetKeyByCode(Settings::Settings::KeyBinds["StreamsAbort"])->KeyName.c_str());
			T6SDK::Drawing::DrawTextAbsolute(buffer3, coords2.x, coords2.y, 1.0f, T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::Center, 0x00);
		}
	}

	static void ShowEncodingProgressOnScreen()
	{
		if (!IsFFmpegEncoding)
			return;

		vec2_t coords = T6SDK::Drawing::GetGridCellCoords(8, 19);
		T6SDK::Drawing::DrawTextAbsolute("^3FFMPEG ENCODING IN PROGRESS", coords.x, coords.y, 1.5f, T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::Center, 0x00);

		vec2_t coords2 = T6SDK::Drawing::GetGridCellCoords(8, 21);
		T6SDK::Drawing::DrawTextAbsolute("^7Encoding incomplete, please wait before starting another recording.", coords2.x, coords2.y, 1.0f, T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::Center, 0x00);
	}

	long HRES;
	static long __stdcall detour_present(IDXGISwapChain* p_swap_chain, UINT sync_interval, UINT flags)
	{
		static bool init = false;
		if (!init)
		{
			if (SUCCEEDED(p_swap_chain->GetDevice(__uuidof(ID3D11Device), (void**)&p_device)))
			{
				p_device->GetImmediateContext(&p_context);
				p_swap_chain->GetDesc(&sd);
				init = true;
			}
			else
				HRES = p_present(p_swap_chain, sync_interval, flags);
		}
		if (StreamShouldIgnoreReshade == false)
		{
			if (T6SDK::Dvars::GetBool(CustomDvars::dvar_streams_noFlash))
			{
				if (!IsAnyOtherStream)
					HRES = p_present(p_swap_chain, sync_interval, flags);
			}
			else
				HRES = p_present(p_swap_chain, sync_interval, flags);
		}


		if (ScreenshotRequested)
		{
			if (RecordingAsAvi)
			{
				ID3D11Texture2D* backBuffer;
				HRESULT hr = p_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<LPVOID*>(&backBuffer));
				if (SUCCEEDED(hr) && backBuffer)
					CaptureBackBufferAndQueueAvi(p_context, backBuffer);
				ScreenshotRequested = false;
				if (backBuffer)
					backBuffer->Release();
			}
			else if (RecordingAsFFmpeg && CurrentPipeTarget)
			{
				// Copy raw BGRA bytes to memory and hand them to the writer thread.
				// FFmpeg encodes them directly; no WIC JPEG step or frame files.
				ID3D11Texture2D* backBuffer = nullptr;
				HRESULT hr = p_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<LPVOID*>(&backBuffer));
				if (SUCCEEDED(hr))
				{
					if (texture_to_save == NULL)
					{
						D3D11_TEXTURE2D_DESC td;
						backBuffer->GetDesc(&td);
						p_device->CreateTexture2D(&td, NULL, &texture_to_save);
					}

					if (texture_to_save)
					{
						p_context->CopyResource(texture_to_save, backBuffer);
						vector<uint8_t> frame{};
						if (SUCCEEDED(RawCapture::CaptureBGRA(p_context, texture_to_save, frame)) && !frame.empty())
						{
							FFmpegPipeTarget* target = CurrentPipeTarget;
							unique_lock<mutex> lock(target->QueueMutex);
							// Backpressure: never drop frames, or the demo would run
							// ahead of the encoded video. Raw frames are large, so the
							// queue cap is small and the render thread waits for space.
							target->QueueSpace.wait(lock, [target]() {
								return target->Queue.size() < MaxQueuedFrames || target->StopRequested || target->WriteFailed;
							});
							if (!target->StopRequested && !target->WriteFailed)
							{
								target->Queue.push_back(std::move(frame));
								target->QueueReady.notify_one();
							}
						}
						else
						{
							CurrentPipeTarget->EncodeFailures++;
						}
					}
					backBuffer->Release();
				}
				ScreenshotRequested = false;
			}
			else
			{
				REFGUID GUID_ContainerFormatJpeg{ 0x19e4a5aa, 0x5662, 0x4fc5, 0xa0, 0xc0, 0x17, 0x58, 0x2, 0x8e, 0x10, 0x57 };
				REFGUID GUID_ContainerFormatPng{ 0x1b7cfaf4, 0x713f, 0x473c, 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf };

				ID3D11Texture2D* backBuffer = nullptr;
				HRESULT hr = p_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<LPVOID*>(&backBuffer));
				if (SUCCEEDED(hr) && backBuffer)
				{
					if (texture_to_save == NULL)
					{
						D3D11_TEXTURE2D_DESC td;
						backBuffer->GetDesc(&td);
						p_device->CreateTexture2D(&td, NULL, &texture_to_save);
					}

					if (texture_to_save)
					{
						p_context->CopyResource(texture_to_save, backBuffer);
						std::vector<uint8_t> pixels{};
						if (SUCCEEDED(RawCapture::CaptureBGRA(p_context, texture_to_save, pixels)) && !pixels.empty())
						{
							D3D11_TEXTURE2D_DESC td{};
							texture_to_save->GetDesc(&td);
							const bool useJpg = T6SDK::Dvars::GetBool(CustomDvars::dvar_streams_JPG);
							AsyncFrameJob job{};
							job.Kind = AsyncFrameKind::Image;
							job.Pixels = std::move(pixels);
							job.Width = td.Width;
							job.Height = td.Height;
							job.Container = useJpg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
							job.Path = std::wstring(filename.begin(), filename.end());
							EnqueueAsyncFrame(std::move(job));
						}
						else
						{
							T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to capture frame %i for image sequence.", frameCount);
						}
					}
					backBuffer->Release();
				}
				ScreenshotRequested = false;
			}
		}

		if (StreamShouldIgnoreReshade == true)
		{
			if (T6SDK::Dvars::GetBool(CustomDvars::dvar_streams_noFlash))
			{
				if (!IsAnyOtherStream)
					HRES = p_present(p_swap_chain, sync_interval, flags);
			}
			else
				HRES = p_present(p_swap_chain, sync_interval, flags);
		}

		return HRES;
	}
	static long __stdcall detour_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
	{
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_DEBUG, false, "STREAMS", "Resizing buffers to %ix%i", Width, Height);
		IsAnyOtherStream = false;
		ReleaseCaptureTexture();
		return p_ResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
	}

	static long __stdcall detour_ResizeTarget(IDXGISwapChain* pSwapChain, const DXGI_MODE_DESC* pNewTargetParameters)
	{
		IsAnyOtherStream = false;
		ReleaseCaptureTexture();
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_DEBUG, false, "STREAMS", "Resizing target to %ix%i", pNewTargetParameters->Width, pNewTargetParameters->Height);
		return p_ResizeTarget(pSwapChain, pNewTargetParameters);
	}

	bool initDX11()
	{
		if (!get_present_pointer())
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to get present pointer");
			return false;
		}

		MH_STATUS status = MH_Initialize();
		if (status != MH_OK)
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to initialize MinHook");
			return false;
		}
		
		//MinHook (used to be used in v1.0.0)
		if (MH_CreateHook(reinterpret_cast<void**>(p_present_target), &detour_present, reinterpret_cast<void**>(&p_present)) != MH_OK) 
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to hook present");
			return false;
		}
		else
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Hooked present => 0x%X", (reinterpret_cast<void**>(p_present_target)));
		}

		if (MH_EnableHook(p_present_target) != MH_OK) 
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to enable present");
			return false;
		}


		//if (kiero::init(kiero::RenderType::D3D11) == kiero::Status::Success)
		//{
		//	kiero::bind((uint16_t)8, (void**)&p_present, detour_present);
		//	T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Hooked present => 0x%X", (reinterpret_cast<void**>(p_present_target)));
		//}
		//else
		//{
		//	T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to init kiero");
		//	return false;
		//}


		if (MH_CreateHook(reinterpret_cast<void**>(p_ResizeBuffers_target), &detour_ResizeBuffers, reinterpret_cast<void**>(&p_ResizeBuffers)) != MH_OK) 
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to hook ResizeBuffers");
			return false;
		}

		if (MH_EnableHook(p_ResizeBuffers_target) != MH_OK) 
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to enable ResizeBuffers");
			return false;
		}

		if (MH_CreateHook(reinterpret_cast<void**>(p_ResizeTarget_target), &detour_ResizeTarget, reinterpret_cast<void**>(&p_ResizeTarget)) != MH_OK) 
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to hook ResizeTarget");
			return false;
		}

		if (MH_EnableHook(p_ResizeTarget_target) != MH_OK) 
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to enable ResizeTarget");
			return false;
		}

		return true;
	}
#pragma endregion

#pragma region Streams methods

	ofstream ExportCameraFile{};
	void RecordCamera()
	{
		if (ExportCameraFile.is_open())
		{
			char exportStringline[256];
			bool isPov = T6SDK::Addresses::DemoPlayback.Value()->CameraMode == T6SDK::DemoCameraMode::NONE;
			if (isPov)
			{
				//FOV;X;Y;Z;PITCH;YAW;ROLL
				sprintf(exportStringline, "%f,%f,%f,%f,%f,%f,%f", T6SDK::Dvars::GetFloat(*T6SDK::Dvars::DvarList::cg_fov),
					T6SDK::Addresses::cg->RefDef.vOrigin.x, T6SDK::Addresses::cg->RefDef.vOrigin.y, T6SDK::Addresses::cg->RefDef.vOrigin.z,
					T6SDK::Addresses::cg->RefDefViewAngles.x, T6SDK::Addresses::cg->RefDefViewAngles.y, T6SDK::Addresses::cg->RefDefViewAngles.z);
			}
			else
			{
				//FOV;X;Y;Z;PITCH;YAW;ROLL
				sprintf(exportStringline, "%f,%f,%f,%f,%f,%f,%f", T6SDK::Dvars::GetFloat(*T6SDK::Dvars::DvarList::cg_fov),
					T6SDK::Addresses::DemoPlayback.Value()->FreeRoamCamera.Origin.x, T6SDK::Addresses::DemoPlayback.Value()->FreeRoamCamera.Origin.y, T6SDK::Addresses::DemoPlayback.Value()->FreeRoamCamera.Origin.z,
					T6SDK::Addresses::DemoPlayback.Value()->FreeRoamCamera.Angles.x, T6SDK::Addresses::DemoPlayback.Value()->FreeRoamCamera.Angles.y, T6SDK::Addresses::DemoPlayback.Value()->FreeRoamCamera.Angles.z);
			}
			ExportCameraFile << exportStringline << "\n";
		}
	}

	void CaptureScreenshot(IMVMStream* currentStream)
	{
		CurrentPipeTarget = nullptr;
		if (RecordingAsFFmpeg)
		{
			// PipeTargets is built in the same order as FFmpegStreams.
			for (size_t i = 0; i < FFmpegStreams.size(); ++i)
			{
				if (FFmpegStreams[i] == currentStream)
				{
					CurrentPipeTarget = PipeTargets[i].get();
					break;
				}
			}
		}
		else
		{
			char formatBuffer[32];
			const bool useJpg = T6SDK::Dvars::GetBool(CustomDvars::dvar_streams_JPG);
			sprintf(formatBuffer, "%s", useJpg ? "JPG" : "PNG");
			char buffer[512];
			sprintf(buffer, "%s\\%s_%i.%s", currentStream->Dir.c_str(), currentStream->Name, frameCount, formatBuffer);
			filename = currentStream->Dir+ "\\" +currentStream->Name+"_"+std::to_string(frameCount)+"."+formatBuffer;
		}
		// Pass state is already applied by Enable(), so one redraw with the
		// screenshot flag set is enough; the old first redraw was captured over.
		ScreenshotRequested = true;
		T6SDK::InternalFunctions::SCR_UpdateScreen(2);
	}
	bool abortingStreams = false;
	void NotifyOnStreamsFinished()
	{
		if (abortingStreams == false)
		{
			if (FFmpegEncodeFailed)
				WinToastHelper::send_toast_notification(L"T6MVM FFMPEG FAILED", L"Encoding failed. Check the .ffmpeg.log file next to the output.");
			else
				WinToastHelper::send_toast_notification(L"T6MVM STREAMS FINISHED", L"Recording Complete!");
			if (Settings::Settings::PostStreamsActions["Notification"] == true && !Settings::Settings::AlertzyKey.empty())
			{
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Streams finished. Sending notification via Alertzy.");
				try
				{
					// Get the current time
					auto now = std::chrono::system_clock::now();
					auto in_time_t = std::chrono::system_clock::to_time_t(now);

					// Format the time as a string (e.g., "2023-10-05_14-30-45")
					std::stringstream ss{};
					ss << std::put_time(std::localtime(&in_time_t), "%B %d %Y at %I:%M %p");
					std::string message = "Streams recording finished on " + ss.str();
					std::string script = "curl -s -X POST https://alertzy.app/send \\ -d \"accountKey=" + Settings::Settings::AlertzyKey + "\" \\ -d \"title=T6MVM\" \\ -d \"message=" + message + "\"";
					std::system(script.c_str());
					T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_SUCCESS, false, "STREAMS", "Notification sent.");
				}
				catch (const char* error)
				{
					T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Notification was not sent dut to an error.");
				}
			}
			if (Settings::Settings::PostStreamsActions["OpenStreamsFolder"] == true)
			{
				//Open folder when streams are done
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Streams finished. Attempting to open the streams folder...");
				std::string cmd = "explorer " + folderToDeleteWhenAborted;
				std::system(cmd.c_str());
			}
		}
	}
	static DWORD WINAPI FFmpegPipeWriterThread(LPVOID param)
	{
		auto* target = static_cast<FFmpegPipeTarget*>(param);
		for (;;)
		{
			vector<uint8_t> frame{};
			{
				unique_lock<mutex> lock(target->QueueMutex);
				target->QueueReady.wait(lock, [target]() { return !target->Queue.empty() || target->StopRequested; });
				if (target->Queue.empty())
				{
					if (target->StopRequested)
						break;
					continue;
				}
				frame = std::move(target->Queue.front());
				target->Queue.pop_front();
			}
			target->QueueSpace.notify_one();
			if (!FFmpeg::WriteFrame(target->Encoder, frame.data(), frame.size()))
			{
				lock_guard<mutex> lock(target->QueueMutex);
				target->WriteFailed = true;
				break;
			}
		}
		return 0;
	}

	static void ClosePipeWriterThreads()
	{
		for (auto& targetPtr : PipeTargets)
		{
			FFmpegPipeTarget* target = targetPtr.get();
			if (target->Thread)
			{
				WaitForSingleObject(target->Thread, INFINITE);
				CloseHandle(target->Thread);
				target->Thread = nullptr;
			}
		}
	}

	static DWORD WINAPI FinishFFmpegPipesThread(LPVOID)
	{
		// Drain the queues, then signal EOF to each process in parallel.
		ClosePipeWriterThreads();
		bool anyFailed = false;
		for (auto& targetPtr : PipeTargets)
		{
			FFmpegPipeTarget* target = targetPtr.get();
			if (target->EncodeFailures > 0)
			{
				anyFailed = true;
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "FFmpeg failed to encode %i frame(s) for %s.", target->EncodeFailures, target->Encoder.OutputPath.c_str());
			}
			if (target->WriteFailed)
			{
				anyFailed = true;
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "FFmpeg stopped accepting frames for %s.", target->Encoder.OutputPath.c_str());
			}
			std::string error;
			if (!FFmpeg::FinishPipeEncoder(target->Encoder, error))
			{
				anyFailed = true;
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "FFmpeg failed for %s: %s", target->Encoder.OutputPath.c_str(), error.c_str());
			}
			else
			{
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_SUCCESS, false, "STREAMS", "Encoded video: %s", target->Encoder.OutputPath.c_str());
			}
		}
		PipeTargets.clear();
		FFmpegStreams.clear();
		if (anyFailed)
			FFmpegEncodeFailed = true;
		NotifyOnStreamsFinished();
		IsFFmpegEncoding = false;
		return 0;
	}

	inline static void StopStreams()
	{
		if (!T6SDK::Dvars::GetBool(CustomDvars::dvar_streams))
			return;
		T6SDK::Addresses::IsDemoPaused.Value() = true;
		ScreenshotRequested = false;
		IsAnyOtherStream = false;
		ReleaseCaptureTexture();
		LocalAddresses::h_TickIncreasing.UnHook();
		// Drain (or discard on abort) all queued frames before finalizing AVI files.
		StopAsyncFrameWriter(abortingStreams);
		IsStreamsRunning = false;

		const int framesCaptured = frameCount;
		frameCount = 0;
		T6SDK::Addresses::DemoPlayback.Value()->DemoHudHidden = false;
		IsStreamsStarted = false;
		/*if(T6SDK::Dvars::GetBool(CustomDvars::dvar_frozenCam))
			T6SDK::Dvars::SetFloat(*T6SDK::Dvars::DvarList::timescale, 1.0f);*/

		for (auto& stream : Streams)
		{
			if (stream->gmav)
			{
				gmav_finish(stream->gmav);
				stream->gmav = 0x00;
			}
		}
		if (ExportCameraFile.is_open())
		{
			ExportCameraFile.close();
		}

		bool ffmpegFinalizingOnThread = false;
		CurrentPipeTarget = nullptr;
		if (RecordingAsFFmpeg && !PipeTargets.empty())
		{
			for (auto& targetPtr : PipeTargets)
			{
				FFmpegPipeTarget* target = targetPtr.get();
				{
					lock_guard<mutex> lock(target->QueueMutex);
					if (abortingStreams)
						target->Queue.clear();
					target->StopRequested = true;
				}
				target->QueueReady.notify_one();
				target->QueueSpace.notify_all();
			}

			if (abortingStreams || framesCaptured <= 0)
			{
				// Abort or an empty take: kill the processes and discard output.
				// Killing the child first unblocks any pending pipe write.
				for (auto& targetPtr : PipeTargets)
					FFmpeg::AbortPipeEncoder(targetPtr->Encoder);
				ClosePipeWriterThreads();
				PipeTargets.clear();
			}
			else
			{
				// Finalizing (waiting for FFmpeg) happens off the game thread.
				IsFFmpegEncoding = true;
				ffmpegFinalizingOnThread = true;
				HANDLE thread = CreateThread(nullptr, 0, &FinishFFmpegPipesThread, nullptr, 0, nullptr);
				if (thread)
					CloseHandle(thread);
				else
					FinishFFmpegPipesThread(nullptr);
			}
		}

		if (!abortingStreams && framesCaptured > 0 && !ffmpegFinalizingOnThread)
		{
			CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)NotifyOnStreamsFinished, nullptr, 0, nullptr);
		}

		FFmpegStreams.clear();
		RecordingAsFFmpeg = false;
		RecordingAsAvi = false;
	}

	int StreamsPassesEnabled = 0;
	static int FrameTimeMeasured = 0;
	static vector<std::chrono::nanoseconds> FrameTimes{};
	static std::chrono::steady_clock::time_point start_time;
	static std::chrono::steady_clock::time_point end_time;

	inline static void Update()
	{
		if (!T6SDK::Dvars::GetBool(CustomDvars::dvar_streams))
			return;
		if (!IsStreamsStarted)
			return;
		if (T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickStart) > -1 && T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickEnd) > -1)
		{
			if (T6SDK::Addresses::Tick.Value() >= T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickEnd))
			{
				StopStreams();
				return;
			}
		}
		if(FrameTimeMeasured < 3)
			start_time = std::chrono::high_resolution_clock::now();
		IsStreamsRunning = true;
		IsAnyOtherStream = false;
		StreamShouldIgnoreReshade = false;
		T6SDK::InternalFunctions::SCR_UpdateScreen(2);
		StreamsPassesEnabled = 0;
		if(T6SDK::Theater::IsInTheater())
		{
			for (auto& stream : Streams)
			{
				if (stream->toggle->current.enabled)
				{
					StreamsPassesEnabled++;
					IsAnyOtherStream = true; //NOFLASH
					StreamShouldIgnoreReshade = stream->IgnoreReshade->current.enabled;
					stream->Enable();
					gmav = stream->gmav;
					CaptureScreenshot(stream);
					stream->Disable();
				}
			}
		}
		if(StreamsPassesEnabled == 0)
			CaptureScreenshot(&Default);

		frameCount++;
		if (T6SDK::Dvars::GetBool(CustomDvars::dvar_streams_recordCam))
		{
			RecordCamera();
		}

		if (FrameTimeMeasured < 3)
		{
			end_time = std::chrono::high_resolution_clock::now();
			auto duration = end_time - start_time;
			FrameTimes.push_back(duration);
			FrameTimeMeasured++;
		}
		else if(FrameTimeMeasured == 3)
		{
			//Calculate average frame time
			auto total_duration = std::chrono::nanoseconds(0);
			for (const auto& frame_time : FrameTimes)
			{
				total_duration += frame_time;
			}
			auto average_duration = total_duration / FrameTimes.size();
			int average_frame_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(average_duration).count();
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Average frame time: %d ms", average_frame_time_ms);
			if (T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickStart) > -1 && T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickEnd) > -1)
			{
				int totalFrames = (T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickEnd) - T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickStart)) / (1000 / T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_fps));
				int estimatedDuration = totalFrames * average_frame_time_ms;
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Approximate estimated duration: %i hour(s) %i minute(s) %i second(s)	", estimatedDuration / 3600000, (estimatedDuration / 60000) % 60, estimatedDuration / 1000);
			}
			FrameTimeMeasured++;
		}

		//T6SDK::ConsoleLog::LogFormatted(CONSOLETEXTGREEN, "Successfully captured %i", frameCount);
	}

	uintptr_t eaxTMP, ecxTMP, edxTMP, esiTMP, ediTMP, espTMP, ebpTMP;
	int tickStep = 0;
	int tickIncreasingEnabled = 0;

	int PrepareTickIncreasing()
	{
		if (!T6SDK::MAIN::ENABLED)
			return 0;
		tickStep = 1000 / T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_fps);
		return 1;
	}

	__declspec(naked) void OnTickIncreasing()
	{
		__asm
		{
			mov[eaxTMP], eax
			mov[edxTMP], edx
			mov[ecxTMP], ecx
			mov[esiTMP], esi
			mov[ediTMP], edi
			mov[espTMP], esp
			mov[ebpTMP], ebp
			call PrepareTickIncreasing
			mov[tickIncreasingEnabled], eax
			mov edx, [edxTMP]
			mov ecx, [ecxTMP]
			mov esi, [esiTMP]
			mov edi, [ediTMP]
			mov esp, [espTMP]
			mov ebp, [ebpTMP]
			cmp [tickIncreasingEnabled], 0
			je TickIncreasingDisabled
			call Update
			mov eax, tickStep
			mov edx, [edxTMP]
			mov ecx, [ecxTMP]
			mov esi, [esiTMP]
			mov edi, [ediTMP]
			mov esp, [espTMP]
			mov ebp, [ebpTMP]
			call[LocalAddresses::InternalTickIncreaseFunc]
			jmp[LocalAddresses::h_TickIncreasing.JumpBackAddress]
		TickIncreasingDisabled:
			call[LocalAddresses::InternalTickIncreaseFunc]
			jmp[LocalAddresses::h_TickIncreasing.JumpBackAddress]
		}
	}

	static bool AddFFmpegPipeTarget(IMVMStream* stream, std::string& error)
	{
		FFmpeg::PipeEncoder encoder{};
		encoder.Executable = Settings::Settings::FFmpegPath;
		encoder.OutputPath = std::string(newStreamFolder) + "\\" + stream->Name + "." + FFmpeg::ContainerExtension(T6SDK::Dvars::GetInt(CustomDvars::dvar_ffmpeg_container));
		encoder.FPS = T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_fps);
		encoder.Width = T6SDK::Addresses::ScreenWidth.Value();
		encoder.Height = T6SDK::Addresses::ScreenHeight.Value();
		encoder.Settings.Codec = T6SDK::Dvars::GetInt(CustomDvars::dvar_ffmpeg_codec);
		encoder.Settings.Quality = T6SDK::Dvars::GetInt(CustomDvars::dvar_ffmpeg_quality);
		encoder.Settings.Preset = T6SDK::Dvars::GetInt(CustomDvars::dvar_ffmpeg_preset);
		encoder.Settings.PixelFormat = T6SDK::Dvars::GetInt(CustomDvars::dvar_ffmpeg_pixelFormat);
		encoder.Settings.Container = T6SDK::Dvars::GetInt(CustomDvars::dvar_ffmpeg_container);
		encoder.Settings.Faststart = T6SDK::Dvars::GetBool(CustomDvars::dvar_ffmpeg_faststart);
		const char* extraArgs = T6SDK::Dvars::GetString(CustomDvars::dvar_ffmpeg_extraArgs);
		if (extraArgs)
			encoder.Settings.ExtraArgs = extraArgs;

		if (!FFmpeg::StartPipeEncoder(encoder, error))
			return false;

		PipeTargets.push_back(make_unique<FFmpegPipeTarget>());
		PipeTargets.back()->Encoder = std::move(encoder);
		FFmpegStreams.push_back(stream);
		return true;
	}

	static void CleanupFailedPipeTargets()
	{
		for (auto& targetPtr : PipeTargets)
		{
			FFmpegPipeTarget* target = targetPtr.get();
			{
				lock_guard<mutex> lock(target->QueueMutex);
				target->StopRequested = true;
			}
			target->QueueReady.notify_one();
			target->QueueSpace.notify_all();
		}
		// Kill the child first: a blocked WriteFile is released when its pipe
		// reader goes away, so the writer threads can then exit.
		for (auto& targetPtr : PipeTargets)
			FFmpeg::AbortPipeEncoder(targetPtr->Encoder);
		ClosePipeWriterThreads();
		PipeTargets.clear();
		FFmpegStreams.clear();
	}

	inline static void StartStreams()
	{
		if (!T6SDK::Dvars::GetBool(CustomDvars::dvar_streams))
			return;

		if (IsFFmpegEncoding)
		{
			T6SDK::Theater::Demo_Error("FFmpeg is still encoding!", "Wait for the previous video to finish before starting a new recording.");
			return;
		}

		const bool useFFmpeg = IsFFmpegMode();
		const bool useAvi = !useFFmpeg && CustomDvars::dvar_streams_avi->current.enabled;
		if (useFFmpeg)
		{
			std::error_code ec;
			if (Settings::Settings::FFmpegPath.empty() ||
				!std::filesystem::is_regular_file(Settings::Settings::FFmpegPath, ec) || ec)
			{
				T6SDK::Theater::Demo_Error("FFmpeg is not configured!", "Select ffmpeg.exe in the Streams tab before recording.");
				return;
			}
		}

		//Creating a new directory for streams
		if (Settings::Settings::StreamsDirectory.empty())
		{
			T6SDK::Theater::Demo_Error("Streams directory is not set!", "Use ^5mvm_streams_directory ^7dvar or use ^5TAB ^7menu to set a directory for streams.");
			return;
		}
		char streamsDirectory[256];
		sprintf(streamsDirectory, "%s\\Streams_%s", Settings::Settings::StreamsDirectory.c_str(), T6SDK::InternalFunctions::getCurrentDateTimeString().c_str());
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_DEBUG, false, "STREAMS", "Creating directory for streams at: %s", streamsDirectory);
		if (!T6SDK::InternalFunctions::CreateNewDirectory(streamsDirectory))
		{
			T6SDK::Theater::Demo_Error("Error occured!", "Failed to create directory for streams!");
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to create directory for streams!");
			StopStreams();
			return;
		}
		StreamsCommon::CapturePreviousDof();
		StreamsCommon::CapturePreviousSsao();
		newStreamFolder = streamsDirectory;
		folderToDeleteWhenAborted = string(streamsDirectory);
		RecordingAsFFmpeg = useFFmpeg;
		RecordingAsAvi = useAvi;
		FFmpegStreams.clear();
		FFmpegEncodeFailed = false;
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "newStreamFolder: %s", newStreamFolder);
		if(T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickStart) > -1)
			T6SDK::Theater::Demo_JumpToTick(T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_tickStart));
		T6SDK::Addresses::DemoPlayback.Value()->DemoHudHidden = true;
		CustomDvars::dvar_greenScreen->current.enabled = false;

		//Create output directories/files for each enabled pass.
		bool anyEnabled = false;
		for (auto& stream : Streams)
		{
			stream->Disable();
			if (!stream->toggle->current.enabled)
				continue;
			anyEnabled = true;

			if (RecordingAsFFmpeg)
			{
				std::string error;
				if (!AddFFmpegPipeTarget(stream, error))
				{
					T6SDK::Theater::Demo_Error("FFmpeg failed to start!", error.c_str());
					T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to start FFmpeg for %s: %s", stream->Name, error.c_str());
					CleanupFailedPipeTargets();
					RecordingAsFFmpeg = false;
					RecordingAsAvi = false;
					return;
				}
			}
			else if (RecordingAsAvi)
			{
				char buffer[256];
				sprintf(buffer, "%s\\%s.avi", newStreamFolder, stream->Name);
				std::string aviPath(buffer);
				stream->gmav = gmav_open(aviPath.c_str(), T6SDK::Addresses::ScreenWidth.Value(), T6SDK::Addresses::ScreenHeight.Value(), T6SDK::Dvars::GetInt(CustomDvars::dvar_streams_fps));
			}
			else
			{
				char buffer[256];
				sprintf(buffer, "%s\\%s", newStreamFolder, stream->Name);
				T6SDK::InternalFunctions::CreateNewDirectory(buffer);
				stream->Dir = buffer;
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "New stream directory for %s", stream->Dir.c_str());
			}
		}

		//If every pass is disabled, Update() falls back to the Default stream.
		if (RecordingAsFFmpeg && !anyEnabled)
		{
			std::string error;
			if (!AddFFmpegPipeTarget(&Default, error))
			{
				T6SDK::Theater::Demo_Error("FFmpeg failed to start!", error.c_str());
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to start FFmpeg for %s: %s", Default.Name, error.c_str());
				CleanupFailedPipeTargets();
				RecordingAsFFmpeg = false;
				RecordingAsAvi = false;
				return;
			}
		}

		// Start writer threads only after every target is in place, so the
		// pointers handed to the threads stay valid.
		if (RecordingAsFFmpeg)
		{
			for (auto& targetPtr : PipeTargets)
			{
				FFmpegPipeTarget* target = targetPtr.get();
				target->Thread = CreateThread(nullptr, 0, &FFmpegPipeWriterThread, target, 0, nullptr);
				if (!target->Thread)
				{
					T6SDK::Theater::Demo_Error("FFmpeg failed to start!", "Could not create the frame writer thread.");
					T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Could not create FFmpeg frame writer thread.");
					CleanupFailedPipeTargets();
					RecordingAsFFmpeg = false;
					RecordingAsAvi = false;
					return;
				}
			}
		}

		if (T6SDK::Dvars::GetBool(CustomDvars::dvar_streams_recordCam))
		{
			bool isPov = T6SDK::Addresses::DemoPlayback.Value()->CameraMode == T6SDK::DemoCameraMode::NONE;
			char CameraFileBuffer[256];
			sprintf(CameraFileBuffer, "%s\\%s_CameraData.t6cr", newStreamFolder, isPov ? "Pov" : "Cine");
			string str(CameraFileBuffer);
			ExportCameraFile.open(str, ios_base::out);
		}

		if (!RecordingAsFFmpeg && !StartAsyncFrameWriter())
		{
			T6SDK::Theater::Demo_Error("Recorder failed to start!", "Could not create the async frame writer thread.");
			StopStreams();
			return;
		}

		LocalAddresses::h_TickIncreasing.Hook(OnTickIncreasing);
		T6SDK::Addresses::IsDemoPaused.Value() = false;
		IsStreamsStarted = true;

		char buffer2[256];
		sprintf(buffer2, "Press %s to stop. Press %s to abort recording and remove recorded files.", 
			T6SDK::Input::GetKeyByCode(Settings::Settings::KeyBinds["StreamsStartStop"])->KeyName.c_str(), 
			T6SDK::Input::GetKeyByCode(Settings::Settings::KeyBinds["StreamsAbort"])->KeyName.c_str());
		// Create converter
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;

		// Convert to wstring
		std::wstring result = converter.from_bytes(buffer2);
		WinToastHelper::send_toast_notification(L"T6MVM STREAMS STARTED", result);
		FrameTimeMeasured = 0;
	}

	inline static void AbortStreams()
	{
		if(!IsStreamsStarted)
			return;
		abortingStreams = true;
		StopStreams();
		abortingStreams = false;
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Attempting to delete the streams folder...");
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_DEBUG, false, "STREAMS", folderToDeleteWhenAborted.c_str());
		try 
		{
			// Check if the folder exists
			if (std::filesystem::exists(folderToDeleteWhenAborted.c_str()))
			{
				// Delete the folder and its contents
				std::filesystem::remove_all(folderToDeleteWhenAborted.c_str());
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_SUCCESS, false, "STREAMS", "Folder deleted successfully");
			}
			else 
			{
				T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_WARNING, false, "STREAMS", "Folder does not exist");
			}
		}
		catch (const std::filesystem::filesystem_error& e)
		{
			// Handle filesystem errors
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Error deleting folder:");
		}
	}

	inline static void StreamsSwitchState() 
	{
		if (!T6SDK::Dvars::GetBool(CustomDvars::dvar_streams))
			return;
		if (IsStreamsRunning)
			StopStreams();
		else
			StartStreams();
	}


	inline static void InitStreams()
	{
		//Default
		Default.Init();
		Streams.push_back(&Default);
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Default stream registered!");

		//NoGun
		NoGun.Init();
		Streams.push_back(&NoGun);
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "No gun stream registered!");

		//GreenScreen
		GreenScreen.Init();
		Streams.push_back(&GreenScreen);
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Green screen stream registered!");

		//NoPlayers
		NoPlayers.Init();
		Streams.push_back(&NoPlayers);
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "No players stream registered!");

		//Depth
		Depth.Init();
		Streams.push_back(&Depth);
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Depth stream registered!");

		//DepthWithGun
		DepthWithGun.Init();
		Streams.push_back(&DepthWithGun);
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Depth with gun stream registered!");

		//FxOnly
		FxOnly.Init();
		Streams.push_back(&FxOnly);
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "FxOnly stream registered!");

		//Green sky
		GreenSky.Init();
		Streams.push_back(&GreenSky);
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "STREAMS", "Green sky stream registered!");


		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_SUCCESS, false, "STREAMS", "Total streams registered: %i", Streams.size());

		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_DEBUG, false, "STREAMS", "Initializing DX11...");
		if (initDX11() == false)
		{
			T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_ERROR, false, "STREAMS", "Failed to initialize DX11!");
			return;
		}
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_SUCCESS, false, "STREAMS", "DX11 initialized.");


		//Init UI for passes
		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_DEBUG, false, "STREAMS", "Initializing streams passes UI...");

		UIControls::UI_StreamsPass1CheckBox = UI_StreamPassNS::UI_StreamPass(&Streams, Streams[0], std::string("DEFAULT"), 12, 7, T6SDK::AnchorPoint::TopLeft);
		UIControls::UI_StreamsPass2CheckBox = UI_StreamPassNS::UI_StreamPass(&Streams, Streams[1], std::string("NO GUN"), 12, 11, T6SDK::AnchorPoint::TopLeft);
		UIControls::UI_StreamsPass3CheckBox = UI_StreamPassNS::UI_StreamPass(&Streams, Streams[2], std::string("GREEN SCREEN"), 12, 15, T6SDK::AnchorPoint::TopLeft);
		UIControls::UI_StreamsPass4CheckBox = UI_StreamPassNS::UI_StreamPass(&Streams, Streams[3], std::string("NO PLAYERS"), 12, 19, T6SDK::AnchorPoint::TopLeft);
		UIControls::UI_StreamsPass5CheckBox = UI_StreamPassNS::UI_StreamPass(&Streams, Streams[4], std::string("DEPTH"), 12, 23, T6SDK::AnchorPoint::TopLeft);
		UIControls::UI_StreamsPass6CheckBox = UI_StreamPassNS::UI_StreamPass(&Streams, Streams[5], std::string("DEPTH WITH GUN"), 12, 27, T6SDK::AnchorPoint::TopLeft);
		UIControls::UI_StreamsPass7CheckBox = UI_StreamPassNS::UI_StreamPass(&Streams, Streams[6], std::string("FX ONLY"), 12, 31, T6SDK::AnchorPoint::TopLeft);
		UIControls::UI_StreamsPass8CheckBox = UI_StreamPassNS::UI_StreamPass(&Streams, Streams[7], std::string("GREEN SKY"), 12, 35, T6SDK::AnchorPoint::TopLeft);

		T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_SUCCESS, false, "STREAMS", "Streams passes UI initialized.");

		T6SDK::Dvars::Cmd_AddCommandInternal("mvm_streams_start", StartStreams, &cmd_streams_start_VAR);
		T6SDK::Dvars::Cmd_AddCommandInternal("mvm_streams_stop", StopStreams, &cmd_streams_stop_VAR);
		T6SDK::Dvars::Cmd_AddCommandInternal("mvm_streams_abort", AbortStreams, &cmd_streams_abort_VAR);
	}
#pragma endregion
}