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

	struct EncodeJob
	{
		std::string Executable{};
		std::string InputPattern{};
		std::string OutputPath{};
		std::string FrameDirectory{};
		int FPS = 50;
		int Codec = FFMPEG_CODEC_H264;
		float Quality = 18.0f;
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
	// values of the mvm_ffmpeg_* dvars carried by the job.
	static std::string BuildVideoArgs(const EncodeJob& job)
	{
		static const char* encoders[] = { "libx264", "libx265", "prores_ks", "utvideo", "libaom-av1", "libvpx-vp9" };
		static const char* presets[] = { "ultrafast", "veryfast", "medium", "slow", "veryslow" };
		static const int cpuUsed[] = { 8, 6, 4, 2, 0 };
		static const char* pixelFormats[] = { "yuv420p", "yuv422p", "yuv444p", "yuv420p10le", "yuv422p10le" };

		const int codec = Clamp(job.Codec, 0, 5);
		const int preset = Clamp(job.Preset, 0, 4);
		const int pixelFormat = Clamp(job.PixelFormat, 0, 4);
		const int crf = Clamp((int)job.Quality, 0, 51);

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

		if (job.Faststart && ContainerSupportsFaststart(job.Container))
			args += " -movflags +faststart";

		if (!job.ExtraArgs.empty())
		{
			args += " ";
			args += job.ExtraArgs;
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

	// Encodes a numbered JPG sequence to a video file using the settings carried
	// by the job. This is intentionally run on a background thread by the
	// streams code because it can take minutes.
	static bool EncodeSequence(const EncodeJob& job, std::string& error)
	{
		error.clear();
		if (job.Executable.empty() || job.InputPattern.empty() || job.OutputPath.empty())
		{
			error = "FFmpeg job is missing an executable, input pattern, or output path.";
			return false;
		}

		const std::string logPath = job.OutputPath + ".ffmpeg.log";
		SECURITY_ATTRIBUTES security{};
		security.nLength = sizeof(security);
		security.lpSecurityDescriptor = nullptr;
		security.bInheritHandle = TRUE;

		HANDLE logHandle = CreateFileA(logPath.c_str(), GENERIC_WRITE | GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (logHandle == INVALID_HANDLE_VALUE)
		{
			error = "Could not create FFmpeg log file: " + logPath;
			return false;
		}

		HANDLE nulHandle = CreateFileA("NUL", GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
		if (nulHandle == INVALID_HANDLE_VALUE)
		{
			CloseHandle(logHandle);
			error = "Could not open NUL for FFmpeg stdin.";
			return false;
		}

		const int fps = job.FPS > 0 ? job.FPS : 50;
		std::string commandLine =
			Quote(job.Executable) +
			" -hide_banner -loglevel error -nostdin -y" +
			" -framerate " + std::to_string(fps) +
			" -start_number 0" +
			" -i " + Quote(job.InputPattern) +
			" -an" + BuildVideoArgs(job) +
			" -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\"" +
			" " + Quote(job.OutputPath);

		STARTUPINFOA startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_HIDE;
		startup.hStdInput = nulHandle;
		startup.hStdOutput = logHandle;
		startup.hStdError = logHandle;

		PROCESS_INFORMATION process{};
		std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
		mutableCommandLine.push_back('\0');
		const BOOL created = CreateProcessA(job.Executable.c_str(), mutableCommandLine.data(),
			nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
		const DWORD createError = created ? ERROR_SUCCESS : GetLastError();

		CloseHandle(nulHandle);
		CloseHandle(logHandle);
		if (!created)
		{
			error = "Could not start FFmpeg (Win32 error " + std::to_string(createError) + ").";
			return false;
		}

		const DWORD waitResult = WaitForSingleObject(process.hProcess, 2 * 60 * 60 * 1000);
		DWORD exitCode = 1;
		bool timedOut = false;
		if (waitResult == WAIT_TIMEOUT)
		{
			timedOut = true;
			TerminateProcess(process.hProcess, 1);
			WaitForSingleObject(process.hProcess, 5000);
		}
		else
		{
			GetExitCodeProcess(process.hProcess, &exitCode);
		}

		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);

		std::error_code ec;
		if (!timedOut && exitCode == 0)
		{
			std::filesystem::remove(logPath, ec);
			if (!job.FrameDirectory.empty())
			{
				std::filesystem::remove_all(job.FrameDirectory, ec);
			}
			return true;
		}

		error = timedOut
			? "FFmpeg timed out. Log: " + logPath
			: "FFmpeg exited with code " + std::to_string(exitCode) + ". Log: " + logPath;
		const std::string logTail = ReadLogTail(logPath);
		if (!logTail.empty())
			error += "\n" + logTail;
		return false;
	}
}
