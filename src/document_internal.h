// This file is part of libfiles.
// Copyright 2026 Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Bounds, status and row delivery shared by native image readers.

#pragma once

#include "libfiles/document.h"
#include "decode_internal.h"

#include <limits>
#include <stdexcept>

namespace libfiles::detail
{
	struct document_stop {};

	struct document_decode
	{
		reader& input;
		document_sink& output;
		const document_options& options;
		document_result& result;
		document_frame frame;

		[[noreturn]] void fail(std::string message)
		{
			result.error = std::move(message);
			throw document_stop{};
		}

		void check_cancel()
		{
			if (output.cancelled())
			{
				result.cancelled = true;
				throw document_stop{};
			}
		}

		void working_bytes(const std::uint64_t bytes)
		{
			if (bytes > options.maximum_working_bytes ||
				bytes > std::numeric_limits<std::size_t>::max())
			{
				result.resource_limit = true;
				fail("the image exceeds the decoder's working-memory limit");
			}
		}

		void begin(document_frame value)
		{
			check_cancel();
			if (value.width == 0 || value.height == 0 ||
				value.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
				value.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
				fail("invalid image dimensions");
			frame = value;
			if (!output.begin_frame(frame))
			{
				result.refused = true;
				throw document_stop{};
			}
		}

		void row(const std::uint32_t y, const std::span<const std::uint8_t> bytes)
		{
			check_cancel();
			if (y >= frame.height || bytes.size() < document_row_bytes(frame.width, frame.storage))
				fail("a decoded row does not fit its image");
			output.row(y, bytes);
		}

		void end()
		{
			output.end_frame(frame);
			++result.info.frames;
		}
	};

	document_storage storage_from_bits(std::uint32_t bits) noexcept;
	void document_pcx(document_decode& decode);
	void document_png(document_decode& decode);
	void document_jpeg(document_decode& decode);
	void document_bmp(document_decode& decode);
	void document_gif(document_decode& decode);
	void document_tiff(document_decode& decode);
	void document_wpg(document_decode& decode);
	void document_psd(document_decode& decode);
	void native_lab_to_bgr(int lightness, int a, int b, std::uint8_t* output);
}
