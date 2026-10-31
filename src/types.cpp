// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: What the shared values of types.h compute rather than hold: the one name a picture format has.

#include "libfiles/types.h"

#include <string>

namespace libfiles
{
	std::string picture_format::name() const
	{
		if (palette) return "palette";

		std::string name;

		switch (chroma)
		{
		case chroma_format::unknown: return {};
		case chroma_format::gray: name = alpha ? "graya" : "gray";
			break;
		case chroma_format::yuv420: name = alpha ? "yuva420" : "yuv420";
			break;
		case chroma_format::yuv422: name = alpha ? "yuva422" : "yuv422";
			break;
		case chroma_format::yuv444: name = alpha ? "yuva444" : "yuv444";
			break;
		case chroma_format::yuv411: name = "yuv411";
			break;
		case chroma_format::yuv410: name = "yuv410";
			break;
		case chroma_format::yuv440: name = "yuv440";
			break;
		case chroma_format::rgb: name = alpha ? "rgba" : "rgb";
			break;
		case chroma_format::cmyk: name = "cmyk";
			break;
		case chroma_format::ycck: name = "ycck";
			break;
		case chroma_format::lab: name = "lab";
			break;
		}

		if (bit_depth > 8) name += " " + std::to_string(bit_depth) + "bit";
		return name;
	}
}
