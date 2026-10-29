// Keyboard shortcuts of the add-on: a key with any of Ctrl, Shift and Alt, as text in the settings ("Ctrl+Shift+F5").
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace camfx
{
	struct shortcut
	{
		uint32_t key = 0; // Windows virtual-key code; 0 for no shortcut
		bool ctrl = false, shift = false, alt = false;

		bool operator==(const shortcut &other) const
		{
			return key == other.key && ctrl == other.ctrl && shift == other.shift && alt == other.alt;
		}
	};

	// Names of the keys that are not a letter, a digit or a function key. Other keys are written as their code, "0xBA".
	struct named_key
	{
		uint32_t key;
		const char *name;
	};

	inline constexpr named_key named_keys[] = {
		{ 0x09, "Tab" }, { 0x0D, "Enter" }, { 0x13, "Pause" }, { 0x14, "CapsLock" }, { 0x20, "Space" }, { 0x21, "PageUp" },
		{ 0x22, "PageDown" }, { 0x23, "End" }, { 0x24, "Home" }, { 0x25, "Left" }, { 0x26, "Up" }, { 0x27, "Right" },
		{ 0x28, "Down" }, { 0x2C, "PrintScreen" }, { 0x2D, "Insert" }, { 0x2E, "Delete" }, { 0x6A, "NumMultiply" },
		{ 0x6B, "NumAdd" }, { 0x6D, "NumSubtract" }, { 0x6E, "NumDecimal" }, { 0x6F, "NumDivide" }, { 0x90, "NumLock" },
		{ 0x91, "ScrollLock" },
	};

	inline std::string key_name(uint32_t key)
	{
		if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
			return std::string(1, char(key));
		if (key >= 0x60 && key <= 0x69)
			return "Num" + std::to_string(key - 0x60);
		if (key >= 0x70 && key <= 0x87)
			return "F" + std::to_string(key - 0x70 + 1);
		for (const named_key &k : named_keys)
			if (k.key == key)
				return k.name;
		char text[8];
		snprintf(text, sizeof(text), "0x%02X", key & 0xFF);
		return text;
	}

	inline bool same_text(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
				return false;
		return true;
	}

	// The key a name stands for (key_name, case-insensitive), or 0.
	inline uint32_t key_from_name(std::string_view name)
	{
		for (uint32_t key = 0x01; key <= 0xFE; ++key)
			if (same_text(name, key_name(key)))
				return key;
		return 0;
	}

	// "Ctrl+Shift+Alt+Key" (only the modifiers used), or "None".
	inline std::string shortcut_text(const shortcut &s)
	{
		if (s.key == 0)
			return "None";
		return std::string(s.ctrl ? "Ctrl+" : "") + (s.shift ? "Shift+" : "") + (s.alt ? "Alt+" : "") + key_name(s.key);
	}

	// Reads a shortcut as shortcut_text writes it, in any case and order of the modifiers. "None" or nothing is no shortcut.
	// A plain number is how earlier versions stored the key, always pressed with Ctrl+Shift. False if it cannot be read.
	inline bool parse_shortcut(std::string_view text, shortcut &out)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
			text.remove_prefix(1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
			text.remove_suffix(1);
		out = {};
		if (text.empty() || same_text(text, "None"))
			return true;
		if (text.find_first_not_of("0123456789") == std::string_view::npos)
		{
			if (text.size() > 3)
				return false;
			const uint32_t key = uint32_t(std::stoul(std::string(text)));
			if (key == 0 || key > 0xFE)
				return false;
			out = { key, true, true, false };
			return true;
		}
		for (;;)
		{
			const size_t plus = text.find('+');
			const std::string_view part = text.substr(0, plus);
			if (plus == std::string_view::npos)
			{
				out.key = key_from_name(part);
				return out.key != 0;
			}
			if (same_text(part, "Ctrl") || same_text(part, "Control"))
				out.ctrl = true;
			else if (same_text(part, "Shift"))
				out.shift = true;
			else if (same_text(part, "Alt"))
				out.alt = true;
			else
				return false;
			text.remove_prefix(plus + 1);
		}
	}
}
