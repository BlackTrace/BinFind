//TODO:
// - Groups
// - Crash when supplying a pattern byte as simply "<" or ">"
// - Crash when matching past total buffer size (eg. with rep_1orMore)

#define _CRT_SECURE_NO_WARNINGS

#include "BinFind.h"

#include <cctype>

#ifdef _MSC_VER
#include <Windows.h>
#endif

BinFindPatternByte::BinFindPatternByte(BinFindPattern* owner, const char* byte)
{
	Owner = owner;

	BinFindPatternByteRepeat repeat = rep_1;
	BinFindPatternByteOperation oper = op_Equal;
	uint8_t value = 0;

	bool valueHigh = false;
	bool valueLow = false;

	const char* p = byte;
	while (*p != '\0') {
		const char c = *(p++);

		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) {
			uint8_t singleChar = 0;

			char byteBuffer[2];
			byteBuffer[0] = c;
			byteBuffer[1] = '\0';
			sscanf(byteBuffer, "%hhx", &singleChar);

			if (!valueHigh) {
				value |= (singleChar << 4);
				valueHigh = true;
			} else if (!valueLow) {
				value |= singleChar;
				valueLow = true;
			} else {
				printf("ERROR: Byte value too big in: '%s'\n", byte);
				return;
			}
			continue;
		}

		if (c == '?') {
			if (valueHigh || valueLow) {
				repeat = rep_0or1;
			} else {
				oper = op_Unknown;
			}

		} else if (c == '<') {
			if (*p == '=') {
				oper = op_LessThanEqual;
			} else {
				oper = op_LessThan;
			}

		} else if (c == '>') {
			if (*p == '=') {
				oper = op_GreaterThanEqual;
			} else {
				oper = op_GreaterThan;
			}

		} else if (c == '*') {
			repeat = rep_0orMore;

		} else if (c == '+') {
			repeat = rep_1orMore;
		}
	}

	if (!valueLow) {
		value >>= 4;
	}

	Info = (repeat << 16) | (oper << 8) | value;
}

bool BinFindPatternByte::MatchesByte(uint8_t input)
{
	BinFindPatternByteOperation oper = GetOperation();
	uint8_t value = GetValue();

	switch (oper) {
	case op_Equal: return input == value;
	case op_Unknown: return true;

	case op_LessThan: return input < value;
	case op_LessThanEqual: return input <= value;

	case op_GreaterThan: return input > value;
	case op_GreaterThanEqual: return input >= value;
	}

	printf("ERROR: Unknown pattern byte operation while matching %02hhx with %02hhx: %d\n", input, value, (int)oper);
	return false;
}

std::tuple<size_t, bool> BinFindPatternByte::Matches(uint8_t* input)
{
	BinFindPatternByteRepeat repeat = GetRepeat();

	if (repeat == rep_1) {
		if (MatchesByte(*input)) {
			return std::make_tuple(1, true);
		}
		return std::make_tuple(0, false);

	} else if (repeat == rep_0or1) {
		if (MatchesByte(*input)) {
			return std::make_tuple(1, true);
		}
		if (std::get<1>(NextInOwner().Matches(input))) {
			return std::make_tuple(0, true);
		}
		return std::make_tuple(0, false);

	} else if (repeat == rep_0orMore) {
		if (MatchesByte(*input)) {
			size_t ret = 0;
			while (MatchesByte(*(input++))) {
				ret++;
			}
			return std::make_tuple(ret, true);
		}
		if (std::get<1>(NextInOwner().Matches(input))) {
			return std::make_tuple(0, true);
		}
		return std::make_tuple(0, false);

	} else if (repeat == rep_1orMore) {
		size_t ret = 0;
		while (MatchesByte(*(input++))) {
			ret++;
		}
		return std::make_tuple(ret, ret > 0);
	}

	printf("ERROR: Unknown pattern byte repeat while matching at %p: %d\n", input, (int)repeat);
	return std::make_tuple(0, false);
}

BinFindPatternByte &BinFindPatternByte::NextInOwner()
{
	return Owner->Bytes[Owner->CurrentOffset + 1];
}

BinFindPattern::BinFindPattern(const char* pattern)
{
	char byteBuffer[16];
	int byteCur = 0;

	const char* p = pattern;
	while (*p != '\0') {
		const char c = *(p++);
		if (c == ' ') {
			byteBuffer[byteCur] = '\0';
			Bytes.push_back(BinFindPatternByte(this, byteBuffer));
			byteCur = 0;
		} else {
			byteBuffer[byteCur++] = (char)toupper(c);
			if (byteCur + 1 >= sizeof(byteBuffer)) {
				printf("ERROR: Trying to read past buffer size, stopping pattern compilation");
				return;
			}
		}
	}

	if (byteCur > 0) {
		byteBuffer[byteCur] = '\0';
		Bytes.push_back(BinFindPatternByte(this, byteBuffer));
	}
}

void BinFindPattern::MatchBegin()
{
	CurrentOffset = 0;
}

std::tuple<size_t, bool> BinFindPattern::MatchesNextByte(uint8_t* input)
{
	if (CurrentOffset >= Bytes.size()) {
		printf("ERROR: Trying to match bytes past pattern size!");
		return std::make_tuple(0, false);
	}

	BinFindPatternByte &byte = Bytes[CurrentOffset];
	auto match = byte.Matches(input);
	if (std::get<1>(match)) {
		if (CurrentOffset == 0) {
			ResultHit = input;
			ResultSize = 0;
		}
		CurrentOffset++;
		size_t sz = std::get<0>(match);
		ResultSize += sz;
		return std::make_tuple(sz, true);
	}

	CurrentOffset = 0;
	return std::make_tuple(0, false);
}

bool BinFindPattern::MatchComplete()
{
	return CurrentOffset == Bytes.size();
}

BinFind::BinFind(uint8_t* buffer, size_t size)
{
	m_buffer = buffer;
	m_size = size;
}

BinFind::~BinFind()
{
}

std::vector<BinFindSection> BinFind::Find(const char* pattern)
{
	std::vector<BinFindSection> ret;

	BinFindPattern compiled(pattern);
	if (compiled.Bytes.size() == 0) {
		return ret;
	}

	compiled.MatchBegin();

	uint8_t* p = m_buffer;
	while ((size_t)(p - m_buffer) < m_size) {
		auto match = compiled.MatchesNextByte(p);

		if (std::get<1>(match)) {
			p += std::get<0>(match);

			if (compiled.MatchComplete()) {
				BinFindSection result;
				result.Pointer = compiled.ResultHit;
				result.Size = compiled.ResultSize;
				ret.push_back(result);

				compiled.MatchBegin();
			}
		} else {
			p++;
		}
	}

	return ret;
}

enum BinFind_ConsoleColor
{
	col_None = 0,
	col_Blue = (1 << 0),
	col_Green = (1 << 1),
	col_Red = (1 << 2),
	col_Bright = (1 << 3),
};

static void BinFind_SetColor(BinFind_ConsoleColor fg = col_None, BinFind_ConsoleColor bg = col_None)
{
#ifdef _MSC_VER
	static HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

	WORD attr = 0;

	if ((fg & col_Blue) != 0) {
		attr |= FOREGROUND_BLUE;
	} else if ((fg & col_Green) != 0) {
		attr |= FOREGROUND_GREEN;
	} else if ((fg & col_Red) != 0) {
		attr |= FOREGROUND_RED;
	} else if ((fg & col_Bright) != 0) {
		attr |= FOREGROUND_INTENSITY;
	} else if (fg == col_None) {
		attr |= FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
	}

	if ((bg & col_Blue) != 0) {
		attr |= BACKGROUND_BLUE;
	} else if ((bg & col_Green) != 0) {
		attr |= BACKGROUND_GREEN;
	} else if ((bg & col_Red) != 0) {
		attr |= BACKGROUND_RED;
	} else if ((bg & col_Bright) != 0) {
		attr |= BACKGROUND_INTENSITY;
	}

	SetConsoleTextAttribute(hConsole, attr);
#else
	//TODO
#endif
}

void BinFind_DumpMemory(uint8_t* buffer, size_t size, std::vector<BinFindSection>* highlights)
{
	/*
	00000000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 2c 20 68 6f  |Hello, world, ho|
	00000010  77 20 61 72 65 20 79 6f  75 20 3a 44 0a           |w are you :D.|
	0000001d
	*/
	size_t offset = 0;

	BinFindSection* highlight = nullptr;

	while (offset < size) {
		printf("%p  ", buffer + offset);

		for (int i = 0; i < 16; i++) {
			if (offset + i >= size) {
				break;
			}

			uint8_t* p = buffer + offset + i;
			if (highlight == nullptr) {
				for (auto &section : *highlights) {
					if (p >= section.Pointer && p < section.Pointer + section.Size) {
						highlight = &section;
						break;
					}
				}
			}

			if (highlight != nullptr) {
				BinFind_SetColor(col_None, col_Green);
			}

			printf("%02hhx", *p);

			if (highlight != nullptr) {
				if (p + 1 >= highlight->Pointer + highlight->Size) {
					highlight = nullptr;
				}
			}

			if (highlight == nullptr) {
				BinFind_SetColor();
			}

			putchar(' ');

			if (i == 7) {
				putchar(' ');
			}

			BinFind_SetColor();
		}
		offset += 16;
		putchar('\n');
	}

	printf("%p\n", buffer + size);
}