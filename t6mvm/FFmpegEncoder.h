#pragma once
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace FFmpeg
{
	// Keep these in sync with the mvm_ffmpeg_* dvars in CustomDvars.h and the
	// enum buttons in FFmpegMenu.h.
	inline constexpr int FFMPEG_CODEC_H264 = 0;
	inline constexpr int FFMPEG_CODEC_H265 = 1;
	inline constexpr int FFMPEG_CODEC_PRORES = 2;
	inline constexpr int FFMPEG_CODEC_UTVIDEO = 3;
	inline constexpr int FFMPEG_CODEC_AV1 = 4;
	inline constexpr int FFMPEG_CODEC_VP9 = 5;

	struct VideoSettings
	{
		int Codec = FFMPEG_CODEC_H264;
		int Quality = 18;
		int Preset = 1;
		int PixelFormat = 0;
		int Container = 0;
		bool Faststart = true;
		std::string ExtraArgs{};
	};

	static int Clamp(int value, int minValue, int maxValue)
	{
		return value < minValue ? minValue : (value > maxValue ? maxValue : value);
	}

	// Returns the file extension (without a dot) for the selected container.
	static const char* ContainerExtension(int container)
	{
		static const char* extensions[] = { "mp4", "mkv", "mov", "webm", "avi" };
		return extensions[Clamp(container, 0, 4)];
	}

	// -movflags is only understood by the MP4/MOV muxers.
	static bool ContainerSupportsFaststart(int container)
	{
		return container == 0 || container == 2;
	}

	// Builds the codec-specific portion of the FFmpeg command line from the
	// values of the mvm_ffmpeg_* dvars.
	static std::string BuildVideoArgs(const VideoSettings& settings)
	{
		static const char* encoders[] = { "libx264", "libx265", "prores_ks", "utvideo", "libaom-av1", "libvpx-vp9" };
		static const char* presets[] = { "ultrafast", "veryfast", "medium", "slow", "veryslow" };
		static const int cpuUsed[] = { 8, 6, 4, 2, 0 };
		static const char* pixelFormats[] = { "yuv420p", "yuv422p", "yuv444p", "yuv420p10le", "yuv422p10le" };

		const int codec = Clamp(settings.Codec, 0, 5);
		const int preset = Clamp(settings.Preset, 0, 4);
		const int pixelFormat = Clamp(settings.PixelFormat, 0, 4);
		const int crf = Clamp(settings.Quality, 0, 51);

		std::string args = " -c:v ";
		args += encoders[codec];

		switch (codec)
		{
		case FFMPEG_CODEC_H264:
		case FFMPEG_CODEC_H265:
			args += " -preset ";
			args += presets[preset];
			args += " -crf ";
			args += std::to_string(crf);
			args += " -pix_fmt ";
			args += pixelFormats[pixelFormat];
			break;
		case FFMPEG_CODEC_PRORES:
			// ProRes is visually lossless; CRF does not apply. prores_ks requires 10-bit 4:2:2.
			args += " -profile:v 3 -pix_fmt yuv422p10le";
			break;
		case FFMPEG_CODEC_UTVIDEO:
			// UtVideo is lossless; quality and preset do not apply.
			args += " -pix_fmt ";
			args += pixelFormats[pixelFormat];
			break;
		case FFMPEG_CODEC_AV1:
			args += " -cpu-used ";
			args += std::to_string(cpuUsed[preset]);
			args += " -crf ";
			args += std::to_string(crf);
			args += " -b:v 0 -strict -2 -pix_fmt ";
			args += pixelFormats[pixelFormat];
			break;
		case FFMPEG_CODEC_VP9:
			args += " -deadline good -cpu-used ";
			args += std::to_string(cpuUsed[preset]);
			args += " -crf ";
			args += std::to_string(crf);
			args += " -b:v 0 -pix_fmt ";
			args += pixelFormats[pixelFormat];
			break;
		}

		if (settings.Faststart && ContainerSupportsFaststart(settings.Container))
			args += " -movflags +faststart";

		if (!settings.ExtraArgs.empty())
		{
			args += " ";
			args += settings.ExtraArgs;
		}

		return args;
	}

	static std::string Quote(const std::string& value)
	{
		return "\"" + value + "\"";
	}

	static std::string ReadLogTail(const std::string& path, size_t maxBytes = 4000)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open())
			return {};

		file.seekg(0, std::ios::end);
		const std::streamoff size = file.tellg();
		if (size <= 0)
			return {};

		const std::streamoff start = size > (std::streamoff)maxBytes ? size - (std::streamoff)maxBytes : 0;
		file.seekg(start, std::ios::beg);
		std::string data((size_t)(size - start), '\0');
		file.read(data.data(), (std::streamsize)data.size());
		return data;
	}

	// One live FFmpeg process reading raw BGRA frames from its stdin. Raw pixels
	// avoid the WIC JPEG encode on the render thread and FFmpeg's JPEG decode.
	struct PipeEncoder
	{
		std::string Executable{};
		std::string OutputPath{};
		int FPS = 50;
		UINT Width = 0;
		UINT Height = 0;
		VideoSettings Settings{};
		HANDLE Process = nullptr;
		HANDLE Stdin = nullptr;
		std::string LogPath{};
	};

	static bool StartPipeEncoder(PipeEncoder& encoder, std::string& error)
	{
		error.clear();
		if (encoder.Executable.empty() || encoder.OutputPath.empty())
		{
			error = "FFmpeg job is missing an executable or output path.";
			return false;
		}
		if (encoder.Width == 0 || encoder.Height == 0)
		{
			error = "FFmpeg job is missing a frame size.";
			return false;
		}

		encoder.LogPath = encoder.OutputPath + ".ffmpeg.log";
		SECURITY_ATTRIBUTES security{};
		security.nLength = sizeof(security);
		security.lpSecurityDescriptor = nullptr;
		security.bInheritHandle = TRUE;

		HANDLE stdinRead = nullptr;
		if (!CreatePipe(&stdinRead, &encoder.Stdin, &security, 0))
		{
			error = "Could not create the FFmpeg stdin pipe (Win32 error " + std::to_string(GetLastError()) + ").";
			return false;
		}
		// Only the child gets the read end; the parent keeps the write end.
		SetHandleInformation(encoder.Stdin, HANDLE_FLAG_INHERIT, 0);

		HANDLE logHandle = CreateFileA(encoder.LogPath.c_str(), GENERIC_WRITE | GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (logHandle == INVALID_HANDLE_VALUE)
		{
			error = "Could not create FFmpeg log file: " + encoder.LogPath;
			CloseHandle(stdinRead);
			CloseHandle(encoder.Stdin);
			encoder.Stdin = nullptr;
			return false;
		}

		const int fps = encoder.FPS > 0 ? encoder.FPS : 50;
		const std::string commandLine =
			Quote(encoder.Executable) +
			" -hide_banner -loglevel error -y" +
			" -thread_queue_size 1024" +
			" -f rawvideo -pixel_format bgra -video_size " +
			std::to_string(encoder.Width) + "x" + std::to_string(encoder.Height) +
			" -framerate " + std::to_string(fps) +
			" -i pipe:0" +
			" -an" + BuildVideoArgs(encoder.Settings) +
			" -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\"" +
			" " + Quote(encoder.OutputPath);

		STARTUPINFOA startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_HIDE;
		startup.hStdInput = stdinRead;
		startup.hStdOutput = logHandle;
		startup.hStdError = logHandle;

		PROCESS_INFORMATION process{};
		std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
		mutableCommandLine.push_back('\0');
		const BOOL created = CreateProcessA(encoder.Executable.c_str(), mutableCommandLine.data(),
			nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
		const DWORD createError = created ? ERROR_SUCCESS : GetLastError();

		CloseHandle(stdinRead);
		CloseHandle(logHandle);
		if (!created)
		{
			CloseHandle(encoder.Stdin);
			encoder.Stdin = nullptr;
			error = "Could not start FFmpeg (Win32 error " + std::to_string(createError) + ").";
			return false;
		}

		encoder.Process = process.hProcess;
		CloseHandle(process.hThread);
		return true;
	}

	static bool WriteFrame(PipeEncoder& encoder, const uint8_t* data, size_t size)
	{
		if (!encoder.Stdin || encoder.Stdin == INVALID_HANDLE_VALUE || !data || size == 0)
			return false;

		size_t written = 0;
		while (written < size)
		{
			const DWORD chunk = (DWORD)(size - written > 0x7FFFFFFF ? 0x7FFFFFFF : size - written);
			DWORD justWritten = 0;
			if (!WriteFile(encoder.Stdin, data + written, chunk, &justWritten, nullptr) || justWritten == 0)
				return false;
			written += justWritten;
		}
		return true;
	}

	// Closing stdin signals EOF; FFmpeg then finalizes the container.
	static bool FinishPipeEncoder(PipeEncoder& encoder, std::string& error)
	{
		error.clear();
		if (encoder.Stdin)
		{
			CloseHandle(encoder.Stdin);
			encoder.Stdin = nullptr;
		}
		if (!encoder.Process)
		{
			error = "No FFmpeg process to wait for.";
			return false;
		}

		const DWORD waitResult = WaitForSingleObject(encoder.Process, 2 * 60 * 60 * 1000);
		DWORD exitCode = 1;
		bool timedOut = false;
		if (waitResult == WAIT_TIMEOUT)
		{
			timedOut = true;
			TerminateProcess(encoder.Process, 1);
			WaitForSingleObject(encoder.Process, 5000);
		}
		else
		{
			GetExitCodeProcess(encoder.Process, &exitCode);
		}
		CloseHandle(encoder.Process);
		encoder.Process = nullptr;

		if (timedOut)
		{
			error = "FFmpeg timed out. Log: " + encoder.LogPath;
			return false;
		}
		if (exitCode != 0)
		{
			error = "FFmpeg exited with code " + std::to_string(exitCode) + ". Log: " + encoder.LogPath;
			const std::string logTail = ReadLogTail(encoder.LogPath);
			if (!logTail.empty())
				error += "\n" + logTail;
			return false;
		}

		std::error_code ec;
		std::filesystem::remove(encoder.LogPath, ec);
		return true;
	}

	static void AbortPipeEncoder(PipeEncoder& encoder)
	{
		// Terminate first: closing the child's pipe reader unblocks any pending
		// WriteFile in the writer thread before we close our own handle.
		if (encoder.Process)
		{
			TerminateProcess(encoder.Process, 1);
			WaitForSingleObject(encoder.Process, 5000);
			CloseHandle(encoder.Process);
			encoder.Process = nullptr;
		}
		if (encoder.Stdin)
		{
			CloseHandle(encoder.Stdin);
			encoder.Stdin = nullptr;
		}
	}
}
