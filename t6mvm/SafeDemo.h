#pragma once
#include <StdInclude.h>
#include "Trace.h"

namespace T6MVM_SafeDemo
{
	// T6SDK's GetDemoBriefData writes an 8-byte value into a 4-byte int in
	// ReadDemoCreateDate(). Reimplement the brief parse with size checks and a
	// correctly sized create-date value.
	inline T6SDK::DemoBriefData Get(const std::string& demoPath)
	{
		T6SDK::DemoBriefData empty;
		std::vector<uint8_t> tags = T6SDK::InternalFunctions::readBytesSimple(demoPath + ".tags");
		std::vector<uint8_t> thumbnail = T6SDK::InternalFunctions::readBytesSimple(demoPath + ".thumbnail");
		if (tags.size() < 0x58 || thumbnail.size() < 0xEB)
		{
			T6MVM_Trace::Write("demo data too small %s", demoPath.c_str());
			return empty;
		}

		try
		{
			int duration = T6SDK::DemoHandler::ReadDemoDuration(thumbnail);
			uint64_t rawDate = 0;
			for (int i = 0; i < 8; ++i)
				rawDate |= (uint64_t)thumbnail[0xE3 + i] << (i * 8);
			uint64_t seconds = rawDate / 2048;
			int createDate = seconds > 0x7FFFFFFF ? 0 : (int)seconds;

			std::string path = demoPath;
			std::string map = T6SDK::DemoHandler::ReadMapName(tags);
			std::string mapUi = T6SDK::DemoHandler::ReadMapMenuSelectMaterial(tags);
			std::string mapFriendly = T6SDK::DemoHandler::ReadMapFriendlyName(tags);
			std::string gameMode = T6SDK::DemoHandler::ReadDemoGameMode(tags);
			std::string author = T6SDK::DemoHandler::ReadDemoAuthor(thumbnail);
			std::string name = T6SDK::DemoHandler::ReadDemoName(thumbnail);
			std::string description = T6SDK::DemoHandler::ReadDemoDescription(thumbnail);
			std::string metadata = T6SDK::DemoHandler::ReadDemoMetadata(tags);
			bool hasMetadata = !metadata.empty();
			T6MVM_Trace::Write("demo parsed %s", demoPath.c_str());
			return T6SDK::DemoBriefData(path, map, mapUi, mapFriendly, gameMode, duration, createDate, author, name, description, hasMetadata, metadata);
		}
		catch (const std::exception& e)
		{
			T6MVM_Trace::Write("demo parse exception %s: %s", demoPath.c_str(), e.what());
			return empty;
		}
	}
}
