// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Encoding a JPEG with libjpeg-turbo, its metadata markers included, and turning one losslessly
// with libjpeg-turbo's transupp. Moved from Diffractor's files_jpeg.cpp.

#include "libfiles/encode.h"
#include "libfiles/exif.h"
#include "encode_internal.h"
#include "photoshop_internal.h"

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#define JPEG_INTERNAL_OPTIONS
#define TRANSFORMS_SUPPORTED 1

#include <jpeglib.h>

extern "C" {
#include <transupp.h>
}

namespace libfiles
{
	using namespace encode_detail;

	namespace
	{
		constexpr std::size_t write_buffer_size = 64u * 1024u;

		constexpr auto XMP_EXIF_MARKER = JPEG_APP0 + 1; // EXIF marker / Adobe XMP marker
		constexpr auto ICC_MARKER = JPEG_APP0 + 2; // ICC profile marker
		constexpr auto IPTC_MARKER = JPEG_APP0 + 13; // IPTC marker / BIM marker

		boolean fill_input_buffer(jpeg_decompress_struct* dinfo)
		{
			static JOCTET fakeEoi[] = {0xFF, JPEG_EOI};
			dinfo->src->next_input_byte = fakeEoi;
			dinfo->src->bytes_in_buffer = 2;
			return FALSE;
		}

		void skip_input_data(jpeg_decompress_struct* dinfo, long num_bytes)
		{
			num_bytes = std::min(static_cast<long>(dinfo->src->bytes_in_buffer), num_bytes);
			dinfo->src->next_input_byte += num_bytes;
			dinfo->src->bytes_in_buffer -= static_cast<std::size_t>(num_bytes);
		}

		void source_noop(jpeg_decompress_struct*)
		{
		}

		// libjpeg's fatal errors leave it by an exception; each use resets the codec first.
		[[noreturn]] void handle_error_exit(const j_common_ptr cinfo)
		{
			char msg[JMSG_LENGTH_MAX];
			(*cinfo->err->format_message)(cinfo, msg);
			throw encode_error(msg);
		}

		// Only purpose is to stop libjpeg writing warnings to stderr. Nothing consumes them, and encoding
		// runs on several threads at once, so there is no shared buffer here to race over.
		void handle_output_message(j_common_ptr)
		{
		}

		// Where libjpeg's fatal errors go while a source's tables are taken: back to the setjmp by longjmp,
		// not out by an exception. MSVC's /EHsc lets an optimized build assume an extern "C" function never
		// throws, and drop a catch whose try holds only libjpeg calls - so the exception escaped the encode
		// it should have let fall back to the quality.
		struct jump_error_mgr
		{
			jpeg_error_mgr pub;
			std::jmp_buf* jump;
			char message[JMSG_LENGTH_MAX];
		};

		[[noreturn]] void jump_error_exit(const j_common_ptr cinfo)
		{
			auto* const err = reinterpret_cast<jump_error_mgr*>(cinfo->err);
			(*cinfo->err->format_message)(cinfo, err->message);
			std::longjmp(*err->jump, 1);
		}
	}

	struct jpeg_encoder::state
	{
		jpeg_compress_struct cinfo{};
		jpeg_error_mgr jerr{};
		jpeg_destination_mgr jdms{};

		std::uint8_t buffer[write_buffer_size]{};
		std::vector<std::uint8_t> result;
		std::uint32_t result_width = 0;
		std::uint32_t result_height = 0;
		std::vector<std::string> notes;
	};

	struct jpeg_transformer::state
	{
		jpeg_decompress_struct dinfo{};
		jpeg_source_mgr mem_source{};
		jpeg_error_mgr jerr{};
	};

	namespace
	{
		void init_buffer(const j_compress_ptr pi)
		{
			auto* const s = static_cast<jpeg_encoder::state*>(pi->client_data);

			pi->dest->next_output_byte = s->buffer;
			pi->dest->free_in_buffer = write_buffer_size;
		}

		void term_buffer(const j_compress_ptr pi)
		{
			auto* const s = static_cast<jpeg_encoder::state*>(pi->client_data);
			s->result.insert(s->result.end(), s->buffer,
			                 s->buffer + static_cast<std::size_t>(pi->dest->next_output_byte - s->buffer));
		}

		boolean empty_buffer(const j_compress_ptr pi)
		{
			auto* const s = static_cast<jpeg_encoder::state*>(pi->client_data);
			s->result.insert(s->result.end(), s->buffer, s->buffer + write_buffer_size);
			pi->dest->next_output_byte = s->buffer;
			pi->dest->free_in_buffer = write_buffer_size;
			return TRUE;
		}

		// Adopt the source file's quantization tables and chroma sampling so a re-encode stays close to a
		// fixed point instead of re-quantizing every block against the quality's tables, and so a 4:4:4
		// source is not silently reduced to 4:2:0. Returns false when the source cannot be matched - or
		// libjpeg fails on it, reading its header or copying its tables to the encoder, which is noted.
		// Nothing alive between the setjmp and a longjmp has a destructor to skip.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611) // setjmp in a function with no C++ object to destroy
#endif
		bool copy_source_encode_params(const j_compress_ptr cinfo, const std::span<const std::uint8_t> source,
		                               std::vector<std::string>& notes)
		{
			if (source.data() == nullptr || source.empty()) return false;

			std::jmp_buf jump;
			jump_error_mgr jerr;
			jerr.jump = &jump;
			jpeg_decompress_struct dinfo = {};
			jpeg_source_mgr src = {};

			dinfo.err = jpeg_std_error(&jerr.pub);
			jerr.pub.error_exit = jump_error_exit;
			jerr.pub.output_message = handle_output_message;

			// The encoder's handler would throw, so its errors come here too while the tables are copied.
			jpeg_error_mgr* const encoder_err = cinfo->err;

			if (setjmp(jump))
			{
				cinfo->err = encoder_err;
				jpeg_destroy_decompress(&dinfo);
				notes.emplace_back(jerr.message);
				return false;
			}

			jpeg_create_decompress(&dinfo);

			src.init_source = source_noop;
			src.fill_input_buffer = fill_input_buffer;
			src.skip_input_data = skip_input_data;
			src.resync_to_restart = jpeg_resync_to_restart;
			src.term_source = source_noop;
			src.bytes_in_buffer = source.size();
			src.next_input_byte = source.data();
			dinfo.src = &src;

			// Only plain 8-bit YCbCr is adopted. Grayscale, CMYK and YCCK sources would also need the
			// encoder's input conversion to change, and a deeper precision would not fit this encoder.
			if (JPEG_HEADER_OK != jpeg_read_header(&dinfo, TRUE) || dinfo.jpeg_color_space != JCS_YCbCr ||
				dinfo.num_components != 3 || dinfo.data_precision != 8)
			{
				jpeg_destroy_decompress(&dinfo);
				return false;
			}

			// Copies tables, sampling and colorspace, but also the source's dimensions - the caller's
			// output extent is the edited one, so restore it along with the encoder's input format.
			const auto cx = cinfo->image_width;
			const auto cy = cinfo->image_height;
			cinfo->err = &jerr.pub;
			jpeg_copy_critical_parameters(&dinfo, cinfo);
			cinfo->err = encoder_err;
			cinfo->image_width = cx;
			cinfo->image_height = cy;
			cinfo->input_components = 4;
			cinfo->in_color_space = JCS_EXT_BGRX;

			jpeg_destroy_decompress(&dinfo);
			return true;
		}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

		// The JPEG format limits a single marker segment to 65533 bytes of payload
		// (the 2-byte length field is counted within the 65535-byte segment).
		constexpr std::size_t max_marker_payload = 65533;

		// An ICC profile is split across APP2 segments, each carrying the "ICC_PROFILE\0" signature
		// followed by a 1-based sequence number and the total segment count.
		constexpr std::size_t icc_seq_header_len = 2;
		constexpr std::size_t icc_max_chunk_len = max_marker_payload - icc_signature.size() - icc_seq_header_len;
		constexpr std::size_t icc_max_segments = 255;

		// The JPEG container cannot carry a payload larger than one segment, so an oversized metadata
		// block is reported rather than dropped: writing the image without the tags, ratings or edit
		// history it arrived with would lose user data with nothing said about it.
		void check_marker_payload(const std::size_t len, const std::string_view what)
		{
			if (len > max_marker_payload)
			{
				throw encode_error(std::format("the {} block is {} bytes, over the {} byte JPEG marker limit",
				                               what, len, max_marker_payload));
			}
		}

		void write_marker_checked(const j_compress_ptr cinfo, const int marker_code,
		                          const std::uint8_t* data, const std::size_t len)
		{
			check_marker_payload(len, "metadata"sv);
			jpeg_write_marker(cinfo, marker_code, data, static_cast<unsigned int>(len));
		}

		// The IPTC records as Photoshop's 8BIM resource 0x0404, padded to an even length.
		std::vector<std::uint8_t> make_photoshop_iptc_resource(const std::span<const std::uint8_t> iptc)
		{
			std::vector<std::uint8_t> result;
			if (iptc.size() > (std::numeric_limits<std::uint32_t>::max)()) throw encode_error("IPTC block is too large");

			result.reserve(4u + 2u + 2u + 4u + iptc.size() + (iptc.size() & 1u));

			if (!iptc.empty())
			{
				constexpr std::array<std::uint8_t, 8> resource_header = {'8', 'B', 'I', 'M', 0x04, 0x04, 0, 0};
				result.insert(result.end(), resource_header.begin(), resource_header.end());
				const auto len = static_cast<std::uint32_t>(iptc.size());
				result.push_back(static_cast<std::uint8_t>(len >> 24));
				result.push_back(static_cast<std::uint8_t>(len >> 16));
				result.push_back(static_cast<std::uint8_t>(len >> 8));
				result.push_back(static_cast<std::uint8_t>(len));
				result.insert(result.end(), iptc.begin(), iptc.end());
				if (iptc.size() & 1u) result.push_back(0);
			}
			return result;
		}

		// A Photoshop resource as the JPEG carries it: a last one that lacks its pad byte gains it.
		struct photoshop_record
		{
			std::span<const std::uint8_t> bytes;
			bool pad = false;

			std::size_t size() const noexcept { return bytes.size() + (pad ? 1u : 0u); }
		};

		std::vector<photoshop_record> photoshop_records(const std::span<const std::uint8_t> resources)
		{
			std::vector<photoshop_record> records;

			for (std::size_t pos = 0; pos < resources.size();)
			{
				const auto resource = detail::read_photoshop_resource(resources.subspan(pos));
				if (!resource) break;
				records.push_back({resources.subspan(pos, resource->size), resource->lacks_pad()});
				pos += resource->size;
			}

			return records;
		}

		void write_photoshop_markers(const j_compress_ptr cinfo, const std::span<const std::uint8_t> iptc,
		                             const std::span<const std::uint8_t> resources, std::vector<std::string>& notes)
		{
			const auto iptc_resource = make_photoshop_iptc_resource(iptc);
			std::vector<photoshop_record> records;
			if (!iptc_resource.empty()) records.push_back({iptc_resource});
			for (const auto& record : photoshop_records(resources)) records.push_back(record);

			std::vector<std::uint8_t> marker;
			auto begin_segment = [&]
			{
				marker.clear();
				marker.insert(marker.end(), photoshop_signature.begin(), photoshop_signature.end());
			};

			begin_segment();

			for (const auto& record : records)
			{
				if (photoshop_signature.size() + record.size() > max_marker_payload)
				{
					notes.push_back(std::format("dropping oversized Photoshop resource ({} bytes)", record.size()));
					continue;
				}

				if (marker.size() + record.size() > max_marker_payload)
				{
					write_marker_checked(cinfo, IPTC_MARKER, marker.data(), marker.size());
					begin_segment();
				}

				marker.insert(marker.end(), record.bytes.begin(), record.bytes.end());
				if (record.pad) marker.push_back(0);
			}

			if (marker.size() > photoshop_signature.size())
				write_marker_checked(cinfo, IPTC_MARKER, marker.data(), marker.size());
		}

		// Write an ICC profile as one or more APP2 segments, following the ICC-in-JPEG convention.
		void write_icc_marker(const j_compress_ptr cinfo, const std::span<const std::uint8_t> icc)
		{
			const std::size_t total_segments = (icc.size() + icc_max_chunk_len - 1) / icc_max_chunk_len;

			for (std::size_t seg = 0; seg < total_segments; ++seg)
			{
				const std::size_t offset = seg * icc_max_chunk_len;
				const std::size_t chunk = std::min(icc_max_chunk_len, icc.size() - offset);

				std::vector<std::uint8_t> marker;
				marker.reserve(icc_signature.size() + icc_seq_header_len + chunk);
				marker.insert(marker.end(), icc_signature.begin(), icc_signature.end());
				marker.push_back(static_cast<std::uint8_t>(seg + 1));
				marker.push_back(static_cast<std::uint8_t>(total_segments));
				marker.insert(marker.end(), icc.data() + offset, icc.data() + offset + chunk);
				jpeg_write_marker(cinfo, ICC_MARKER, marker.data(), static_cast<unsigned int>(marker.size()));
			}
		}

		// Everything that cannot be embedded is rejected before compression begins, so a refused file
		// leaves the encoder untouched and ready for the next one.
		void check_metadata_fits(const embedded_metadata& metadata, std::vector<std::string>& notes)
		{
			if (!metadata.exif.empty())
			{
				check_marker_payload(is_exif_signature(metadata.exif)
					                     ? metadata.exif.size()
					                     : exif_signature.size() + metadata.exif.size(), "exif"sv);
			}

			if (!metadata.iptc.empty() || !metadata.photoshop_resources.empty())
			{
				const auto iptc_resource = make_photoshop_iptc_resource(metadata.iptc);

				for (const auto& record : photoshop_records(metadata.photoshop_resources))
				{
					if (photoshop_signature.size() + record.size() > max_marker_payload)
					{
						notes.push_back(std::format("dropping oversized Photoshop resource ({} bytes)", record.size()));
					}
				}

				if (!iptc_resource.empty())
					check_marker_payload(photoshop_signature.size() + iptc_resource.size(), "iptc"sv);
			}
			if (!metadata.xmp.empty()) check_marker_payload(xmp_signature.size() + metadata.xmp.size(), "xmp"sv);

			if (!metadata.icc.empty())
			{
				const auto icc_segments = (metadata.icc.size() + icc_max_chunk_len - 1) / icc_max_chunk_len;

				if (icc_segments > icc_max_segments)
				{
					throw encode_error(std::format(
						"the icc profile is {} bytes, needing {} of the {} APP2 segments JPEG allows",
						metadata.icc.size(), icc_segments, icc_max_segments));
				}
			}
		}

		// A copy of an EXIF block, "Exif\0\0" first, whose orientation is 1 and whose sizes are the image's:
		// a turned image is already the way up the orientation asked for.
		std::vector<std::uint8_t> fix_dims(const std::span<const std::uint8_t> block, const std::uint32_t width,
		                                   const std::uint32_t height)
		{
			std::vector<std::uint8_t> result(block.begin(), block.end());
			if (result.size() <= exif_signature.size()) return result;

			const exif_number numbers[] = {
				{exif_directory::primary, 0x0112, 1}, // Orientation
				{exif_directory::thumbnail, 0x0112, 1},
				{exif_directory::exif, 0xa002, width}, // PixelXDimension
				{exif_directory::exif, 0xa003, height}, // PixelYDimension
				{exif_directory::interoperability, 0x1001, width}, // RelatedImageWidth
				{exif_directory::interoperability, 0x1002, height}, // RelatedImageLength
			};

			set_exif_numbers({result.data() + exif_signature.size(), result.size() - exif_signature.size()}, numbers);
			return result;
		}

		// Indexed by the EXIF orientation less one: 1 is no turn, and 9 is past the end of the table.
		JXFORM_CODE to_transform(const std::uint16_t orientation)
		{
			static constexpr JXFORM_CODE transform[] = {
				JXFORM_NONE, // 1
				JXFORM_FLIP_H,
				JXFORM_ROT_180,
				JXFORM_FLIP_V,
				JXFORM_TRANSPOSE,
				JXFORM_ROT_90,
				JXFORM_TRANSVERSE,
				JXFORM_ROT_270, // 8
			};

			return orientation <= 1 || static_cast<std::size_t>(orientation) > std::size(transform)
				       ? JXFORM_NONE
				       : transform[orientation - 1];
		}
	}

	jpeg_encoder::jpeg_encoder() : _state(std::make_unique<state>())
	{
		_state->cinfo.err = jpeg_std_error(&_state->jerr);
		_state->jerr.error_exit = handle_error_exit;
		_state->jerr.output_message = handle_output_message;
		try
		{
			jpeg_create_compress(&_state->cinfo);
		}
		catch (...)
		{
			jpeg_destroy_compress(&_state->cinfo);
			throw;
		}

		_state->cinfo.dest = &_state->jdms;
		_state->cinfo.client_data = _state.get();

		_state->jdms.init_destination = init_buffer;
		_state->jdms.empty_output_buffer = empty_buffer;
		_state->jdms.term_destination = term_buffer;
	}

	jpeg_encoder::~jpeg_encoder()
	{
		try
		{
			jpeg_destroy_compress(&_state->cinfo);
		}
		catch (...)
		{
		}
	}

	void jpeg_encoder::setup(const std::uint32_t width, const std::uint32_t height, const jpeg_options& options)
	{
		// The encoder is a long-lived object reused for every image, and handle_error_exit throws out of
		// libjpeg. A compress abandoned mid-scan leaves global_state at CSTATE_SCANNING, which makes the
		// jpeg_set_defaults below ERREXIT for the rest of the session. Safe to call in any state.
		jpeg_abort_compress(&_state->cinfo);

		_state->result.clear();

		_state->cinfo.image_width = width;
		_state->cinfo.image_height = height;
		_state->cinfo.input_components = 4;
		_state->cinfo.in_color_space = JCS_EXT_BGRX;

		if (!copy_source_encode_params(&_state->cinfo, options.source, _state->notes))
		{
			jpeg_set_defaults(&_state->cinfo);
			jpeg_set_quality(&_state->cinfo, options.quality, TRUE);
			jpeg_set_colorspace(&_state->cinfo, JCS_YCbCr);
		}

		// Last, because jpeg_set_defaults resets it and the copy above runs it internally.
		_state->cinfo.dct_method = JDCT_ISLOW;
	}

	void jpeg_encoder::start(const std::uint32_t width, const std::uint32_t height, const std::uint16_t orientation,
	                         const embedded_metadata& metadata, const jpeg_options& options)
	{
		_state->notes.clear();
		check_metadata_fits(metadata, _state->notes);

		setup(width, height, options);
		jpeg_start_compress(&_state->cinfo, TRUE);

		_state->result_width = _state->cinfo.jpeg_width;
		_state->result_height = _state->cinfo.jpeg_height;

		if (!metadata.exif.empty())
		{
			if (is_exif_signature(metadata.exif))
			{
				write_marker_checked(&_state->cinfo, XMP_EXIF_MARKER, metadata.exif.data(), metadata.exif.size());
			}
			else
			{
				std::vector<std::uint8_t> marker;
				marker.reserve(exif_signature.size() + metadata.exif.size());
				marker.insert(marker.begin(), exif_signature.begin(), exif_signature.end());
				marker.insert(marker.end(), metadata.exif.begin(), metadata.exif.end());
				write_marker_checked(&_state->cinfo, XMP_EXIF_MARKER, marker.data(), marker.size());
			}
		}
		else if (orientation != 1 && orientation != 0)
		{
			auto exif = orientation_exif(orientation);

			exif.insert(exif.begin(), exif_signature.begin(), exif_signature.end());
			write_marker_checked(&_state->cinfo, XMP_EXIF_MARKER, exif.data(), exif.size());
		}

		if (!metadata.iptc.empty() || !metadata.photoshop_resources.empty())
		{
			write_photoshop_markers(&_state->cinfo, metadata.iptc, metadata.photoshop_resources, _state->notes);
		}

		if (!metadata.xmp.empty())
		{
			std::vector<std::uint8_t> marker;
			marker.reserve(xmp_signature.size() + metadata.xmp.size());
			marker.insert(marker.begin(), xmp_signature.begin(), xmp_signature.end());
			marker.insert(marker.end(), metadata.xmp.begin(), metadata.xmp.end());
			write_marker_checked(&_state->cinfo, XMP_EXIF_MARKER, marker.data(), marker.size());
		}

		if (!metadata.icc.empty())
		{
			write_icc_marker(&_state->cinfo, metadata.icc);
		}
	}

	void jpeg_encoder::write_rows(std::uint8_t** rows, const std::uint32_t count) const
	{
		jpeg_write_scanlines(&_state->cinfo, rows, count);
	}

	std::uint32_t jpeg_encoder::next_row() const noexcept
	{
		return _state->cinfo.next_scanline;
	}

	std::vector<std::uint8_t> jpeg_encoder::complete(const bool can_abort)
	{
		if (can_abort && _state->cinfo.next_scanline < _state->cinfo.image_height)
		{
			jpeg_abort_compress(&_state->cinfo);
		}
		else
		{
			jpeg_finish_compress(&_state->cinfo);
		}
		// complete() is terminal for this image, so hand the buffer out rather than copying it.
		return std::move(_state->result);
	}

	std::vector<std::uint8_t> jpeg_encoder::encode(const std::uint32_t width, const std::uint32_t height,
	                                               const std::uint8_t* pixels, const std::size_t stride,
	                                               const std::uint16_t orientation,
	                                               const embedded_metadata& metadata, const jpeg_options& options)
	{
		start(width, height, orientation, metadata, options);

		std::vector<std::uint8_t*> rows(height);

		for (std::uint32_t i = 0; i < height; i++)
		{
			rows[i] = const_cast<std::uint8_t*>(pixels + stride * i);
		}

		// jpeg_write_scanlines may consume fewer rows than offered, so always resume
		// from next_scanline rather than resubmitting the array from row 0.
		while (_state->cinfo.next_scanline < height)
		{
			const auto next = _state->cinfo.next_scanline;
			write_rows(rows.data() + next, height - next);
		}

		return complete();
	}

	std::uint32_t jpeg_encoder::width() const noexcept
	{
		return _state->result_width;
	}

	std::uint32_t jpeg_encoder::height() const noexcept
	{
		return _state->result_height;
	}

	const std::vector<std::string>& jpeg_encoder::notes() const noexcept
	{
		return _state->notes;
	}

	jpeg_transformer::jpeg_transformer() : _state(std::make_unique<state>())
	{
		_state->dinfo.err = jpeg_std_error(&_state->jerr);
		_state->jerr.error_exit = handle_error_exit;
		_state->jerr.output_message = handle_output_message;
		try
		{
			jpeg_create_decompress(&_state->dinfo);
		}
		catch (...)
		{
			jpeg_destroy_decompress(&_state->dinfo);
			throw;
		}

		_state->dinfo.src = &_state->mem_source;
		_state->dinfo.client_data = this;

		_state->mem_source.init_source = source_noop;
		_state->mem_source.fill_input_buffer = fill_input_buffer;
		_state->mem_source.skip_input_data = skip_input_data;
		_state->mem_source.resync_to_restart = jpeg_resync_to_restart;
		_state->mem_source.term_source = source_noop;
		_state->mem_source.bytes_in_buffer = 0;
		_state->mem_source.next_input_byte = nullptr;
	}

	jpeg_transformer::~jpeg_transformer()
	{
		try
		{
			jpeg_destroy_decompress(&_state->dinfo);
		}
		catch (...)
		{
		}
	}

	bool jpeg_transformer::read_header(const std::span<const std::uint8_t> jpeg)
	{
		// The object is reused for every image. A stream that suspended part way through its header, or a
		// turn a throw abandoned, leaves parse state behind that this read would otherwise resume into.
		jpeg_abort_decompress(&_state->dinfo);

		_state->mem_source.bytes_in_buffer = jpeg.size();
		_state->mem_source.next_input_byte = jpeg.data();
		_state->dinfo.src = &_state->mem_source;

		return JPEG_HEADER_OK == jpeg_read_header(&_state->dinfo, TRUE);
	}

	std::uint32_t jpeg_transformer::width() const noexcept
	{
		return _state->dinfo.image_width;
	}

	std::uint32_t jpeg_transformer::height() const noexcept
	{
		return _state->dinfo.image_height;
	}

	std::int64_t jpeg_transformer::coefficient_bytes() const
	{
		const auto& dinfo = _state->dinfo;

		// In lossless mode a data unit is one sample rather than an 8x8 block, so width_in_blocks is the
		// image width and no coefficient array of this shape exists. block_size carries the data unit
		// from the first SOF and is not touched by the IDCT scaling jdmaster applies later.
		if (dinfo.comp_info == nullptr || dinfo.num_components <= 0 || dinfo.block_size != DCTSIZE) return 0;

		// Both jdcoefct and transupp pad the array to a whole number of sampling blocks each way.
		const auto round_up = [](const std::int64_t value, const std::int64_t to)
		{
			return to > 0 ? (value + to - 1) / to * to : value;
		};

		std::int64_t result = 0;

		for (auto ci = 0; ci < dinfo.num_components; ++ci)
		{
			const auto& comp = dinfo.comp_info[ci];

			result += round_up(comp.width_in_blocks, comp.h_samp_factor) *
				round_up(comp.height_in_blocks, comp.v_samp_factor) * static_cast<std::int64_t>(sizeof(JBLOCK));
		}

		return result;
	}

	lossless_turn jpeg_transformer::turn(const std::span<const std::uint8_t> jpeg, jpeg_encoder& encoder,
	                                     const std::uint16_t orientation, const std::uint64_t budget) const
	{
		lossless_turn result;

		// Both objects are long-lived members of the caller, and every libjpeg call below can throw out
		// of handle_error_exit. Reset on entry and on every exit so one unreadable file cannot leave the
		// decoder or the encoder mid-operation, which would fail every later decode and save.
		jpeg_abort_decompress(&_state->dinfo);
		jpeg_abort_compress(&encoder._state->cinfo);

		const on_exit reset_codecs([this, &encoder]
		{
			jpeg_abort_decompress(&_state->dinfo);
			jpeg_abort_compress(&encoder._state->cinfo);
		});

		const auto transform = to_transform(orientation);
		_state->mem_source.bytes_in_buffer = jpeg.size();
		_state->mem_source.next_input_byte = jpeg.data();

		constexpr JCOPY_OPTION copyoption = JCOPYOPT_ALL;

		jpeg_transform_info transformoption = {}; // image transformation options
		// perfect refuses a turn that would land on a partial MCU. The alternative, trimming, drops up to
		// 15 edge pixels the user could see in the preview; the caller re-encodes those instead.
		transformoption.perfect = TRUE;
		transformoption.slow_hflip = FALSE;
		transformoption.transform = transform;
		transformoption.trim = FALSE;
		transformoption.force_grayscale = FALSE;
		transformoption.crop = FALSE;
		transformoption.crop_width_set = JCROP_UNSET;
		transformoption.crop_height_set = JCROP_UNSET;
		transformoption.crop_xoffset_set = JCROP_UNSET;
		transformoption.crop_yoffset_set = JCROP_UNSET;

		// Enable saving of extra markers that we want to copy
		jcopy_markers_setup(&_state->dinfo, copyoption);

		// Read file header
		if (JPEG_HEADER_OK == jpeg_read_header(&_state->dinfo, TRUE))
		{
			result.width = _state->dinfo.image_width;
			result.height = _state->dinfo.image_height;

			// Any space needed by a transform option must be requested before jpeg_read_coefficients so
			// that memory allocation will be done right. Requesting it allocates nothing yet, so the
			// budget below still comes before anything large.
			if (!jtransform_request_workspace(&_state->dinfo, &transformoption))
			{
				// Refused as imperfect. An empty result tells the caller to take the re-encode path.
				return result;
			}

			// jpeg_read_coefficients holds every coefficient of the full-resolution image, and transupp
			// requests a workspace array the same size for every transform but a plain horizontal flip.
			// Neither shrinks the way a decode does, so the scaled budget a decode applies says nothing
			// about a turn - and without this a hundred-megapixel photograph reached libjpeg asking for
			// gigabytes. Asked after the perfect test, so the caller is told why only for a turn that
			// could have been lossless: re-encoding that instead would hold the decoded picture and a
			// turned copy, more than this was refused for, and lose quality besides.
			result.workspace_bytes = coefficient_bytes() * 2;

			if (static_cast<std::uint64_t>(result.workspace_bytes) > budget)
			{
				result.over_budget = true;
				return result;
			}

			constexpr jpeg_options encode_options;

			encoder.setup(_state->dinfo.image_width, _state->dinfo.image_height, encode_options);

			// Read source file as DCT coefficients
			auto* const src_coef_arrays = jpeg_read_coefficients(&_state->dinfo);

			// Null means libjpeg suspended. The source manager offers the whole file at once, so that
			// only happens when the entropy data ends early, and transupp indexes this array unchecked.
			if (src_coef_arrays == nullptr)
			{
				return result;
			}

			auto& cinfo = encoder._state->cinfo;

			// Initialize destination compression parameters from source values
			jpeg_copy_critical_parameters(&_state->dinfo, &cinfo);

			// Adjust destination parameters if required by transform options;
			// also find out which set of coefficient arrays will hold the output.
			auto* const dst_coef_arrays = jtransform_adjust_parameters(&_state->dinfo, &cinfo,
			                                                           src_coef_arrays, &transformoption);

			// Start compressor (note no image data is actually written here)
			jpeg_write_coefficients(&cinfo, dst_coef_arrays);

			// Example of code that also updates thumbnail
			// https://github.com/kraxel/fbida/blob/master/jpegtools.c
			//
			for (auto* marker = _state->dinfo.marker_list; marker != nullptr; marker = marker->next)
			{
				const std::span<const std::uint8_t> block = {marker->data, marker->data_length};

				if (marker->marker == XMP_EXIF_MARKER && is_exif_signature(block))
				{
					const auto fixed = fix_dims(block, cinfo.image_width, cinfo.image_height);
					marker->data = static_cast<std::uint8_t*>(_state->dinfo.mem->alloc_large(
						reinterpret_cast<j_common_ptr>(&_state->dinfo), JPOOL_IMAGE, fixed.size()));
					marker->original_length = static_cast<unsigned int>(fixed.size());
					marker->data_length = static_cast<unsigned int>(fixed.size());
					std::memcpy(marker->data, fixed.data(), fixed.size());
				}
			}

			// Copy to the output file any extra markers that we want to preserve
			jcopy_markers_execute(&_state->dinfo, &cinfo, copyoption);

			// Execute image transformation, if any
			jtransform_execute_transform(&_state->dinfo, &cinfo, src_coef_arrays, &transformoption);

			result.data = encoder.complete(false);
			jpeg_finish_decompress(&_state->dinfo);
		}

		return result;
	}
}
