#pragma once
#include <StdInclude.h>
#include "Settings.h"
#include <filesystem>
#include <vector>
#include <string>

// Drawn from the DX11 Present detour because T6SDK suppresses all of its own
// UI outside theater mode. This is the only way to offer a demo picker at the
// main menu, where Redacted's dead master server otherwise blocks Theater.
namespace MainMenuLauncher
{
	inline static bool Visible = true;
	inline static bool LastLButton = false;
	inline static bool LastToggle = false;
	inline static bool LastRefresh = false;
	inline static bool Scanned = false;
	inline static std::vector<std::string> DemoPaths{};

	static void RefreshDemos()
	{
		DemoPaths.clear();
		std::string dir = Settings::Settings::GetDemosDirectory();
		if (dir.empty())
		{
			const char* home = T6SDK::Dvars::GetString(*T6SDK::Dvars::DvarList::fs_homepath);
			if (home)
				dir = std::string(home) + "\\Plugins\\OpenNetStorage\\Cloud\\FILM_MP\\";
		}
		if (dir.empty())
			return;

		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
		{
			if (ec)
				break;
			if (!entry.is_regular_file(ec))
				continue;
			if (entry.path().extension() != ".demo")
				continue;
			// LoadDemoFromFile needs the summary and thumbnail files.
			if (!std::filesystem::exists(entry.path().string() + ".summary", ec) ||
				!std::filesystem::exists(entry.path().string() + ".thumbnail", ec))
				continue;
			DemoPaths.push_back(entry.path().string());
		}
		std::sort(DemoPaths.begin(), DemoPaths.end());
	}

	static void Draw(HWND hwnd)
	{
		const bool toggle = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
		if (toggle && !LastToggle)
		{
			Visible = !Visible;
			if (Visible)
			{
				RefreshDemos();
				Scanned = true;
			}
		}
		LastToggle = toggle;

		const bool refresh = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
		if (refresh && !LastRefresh)
			RefreshDemos();
		LastRefresh = refresh;

		if (!Visible)
			return;
		if (!Scanned)
		{
			RefreshDemos();
			Scanned = true;
		}

		const float scale = 1.0f / 1080.0f * static_cast<float>(T6SDK::Addresses::ScreenHeight.Value());

		vec2_t title = T6SDK::Drawing::GetGridCellCoords(0, 6);
		T6SDK::Drawing::DrawTextAbsolute("^5T6MVM ^7DEMO LAUNCHER  ^3F9 ^7hide  ^3F5 ^7refresh",
			title.x, title.y, scale * 1.2f, T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::TopLeft, nullptr);

		vec2_t hint = T6SDK::Drawing::GetGridCellCoords(0, 7);
		T6SDK::Drawing::DrawTextAbsolute("Click a demo to load it straight into theater (no Online tab needed).",
			hint.x, hint.y, scale, T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::TopLeft, nullptr);

		if (DemoPaths.empty())
		{
			vec2_t empty = T6SDK::Drawing::GetGridCellCoords(0, 9);
			T6SDK::Drawing::DrawTextAbsolute("No demos found in the demos directory.",
				empty.x, empty.y, scale, T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::TopLeft, nullptr);
			return;
		}

		const bool lbutton = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		const bool click = lbutton && !LastLButton;
		LastLButton = lbutton;

		POINT cursor{};
		GetCursorPos(&cursor);
		ScreenToClient(hwnd, &cursor);

		const size_t shown = DemoPaths.size() < 12 ? DemoPaths.size() : 12;
		for (size_t i = 0; i < shown; ++i)
		{
			vec2_t row = T6SDK::Drawing::GetGridCellCoords(0, 9 + static_cast<int>(i));
			std::string name = std::filesystem::path(DemoPaths[i]).stem().string();
			if (name.size() > 42)
				name = name.substr(0, 42);
			std::string label = std::to_string(i + 1) + ". " + name;

			RECT textRect{};
			T6SDK::Drawing::DrawTextAbsolute(label.c_str(), row.x, row.y, scale,
				T6SDK::Drawing::WHITECOLOR, T6SDK::AnchorPoint::TopLeft, &textRect);

			if (click && PtInRect(&textRect, cursor))
			{
				T6SDK::DemoHandler::LoadDemoFromFile(DemoPaths[i].c_str());
				break;
			}
		}
	}
}
