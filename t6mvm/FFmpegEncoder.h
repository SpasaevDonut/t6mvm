#pragma once
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace FFmpeg
{
	struct EncodeJob
	{
		std::string Executable{};
		std::string InputPattern{};
		std::string OutputPath{};
		std::string FrameDirectory{};
		int FPS = 50;
	};

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

	// Encodes a numbered JPG sequence to H.264 MP4. This is intentionally run
	// on a background thread by the streams code because it can take minutes.
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
			" -an -c:v libx264 -preset veryfast -crf 18" +
			" -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\"" +
			" -pix_fmt yuv420p -movflags +faststart" +
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
