#pragma once
#include <cstdarg>
#include <cstdio>

namespace T6MVM_Trace
{
	inline void Write(const char* format, ...)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, "C:\\t6mvm_trace.log", "a") != 0 || !file)
			return;

		va_list args;
		va_start(args, format);
		vfprintf(file, format, args);
		va_end(args);
		fputc('\n', file);
		fclose(file);
	}
}
