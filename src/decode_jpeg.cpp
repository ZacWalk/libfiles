// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Decoding a JPEG with libjpeg-turbo - baseline, extended and progressive, lossless up to 16
// bits, in YCbCr, RGB, grayscale, CMYK and YCCK - reduced by 2, 4 or 8 while it decodes when the caller
// shows it smaller, to B8G8R8X8, or to NV12 for a GPU to convert. Moved from Diffractor's decoder,
// which shipped it, call for call, so that what it writes is what Diffractor's wrote, byte for byte,
// for a file cut short too.

#include "decode_internal.h"
#include "document_internal.h"

#include <cstddef>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

#include <jpeglib.h>

namespace libfiles::detail
{
	namespace
	{
		// Rows asked of libjpeg at a time. NV12's raw reads must take a whole iMCU row at once, so the
		// sampling that path accepts is capped against this.
		constexpr std::uint32_t max_chunk = 2 * DCTSIZE;
		constexpr int app1_marker = JPEG_APP0 + 1;

		// Where pixel_format::bgrx8 keeps each colour, as libjpeg-turbo's JCS_EXT_BGRX writes it.
		constexpr std::size_t bgrx_blue = 0;
		constexpr std::size_t bgrx_green = 1;
		constexpr std::size_t bgrx_red = 2;
		constexpr std::size_t stream_block = 64 * 1024;

		class jpeg_failure final : public std::runtime_error
		{
		public:
			using std::runtime_error::runtime_error;
		};

		// libjpeg's fatal errors leave the decompressor by an exception, which decode_jpeg catches. The
		// decompressor is reset before its next use.
		[[noreturn]] void error_exit(const j_common_ptr cinfo)
		{
			char message[JMSG_LENGTH_MAX];
			(*cinfo->err->format_message)(cinfo, message);
			throw jpeg_failure(message);
		}

		// Warnings go nowhere: decoding runs on many threads at once, and nothing reads them.
		void output_message(j_common_ptr)
		{
		}
	}

	struct jpeg_state
	{
		jpeg_decompress_struct dinfo{};
		jpeg_error_mgr jerr{};
		jpeg_source_mgr source{};
		bool created = false;

		// A file that is not resident is read a block at a time, from next.
		reader* file = nullptr;
		std::uint64_t next = 0;
		std::vector<std::uint8_t> block;

		// The data ran out, and libjpeg was given an end of image in its place.
		bool ended = false;
		bool suspend_at_eof = true;
		std::vector<std::string>* warnings = nullptr;
		std::jmp_buf* jump = nullptr;
		char message[JMSG_LENGTH_MAX]{};
	};

	void jpeg_state_deleter::operator()(jpeg_state* const state) const noexcept
	{
		if (state && state->created) jpeg_destroy_decompress(&state->dinfo);
		delete state;
	}

	namespace
	{
		jpeg_state& state_of(const j_decompress_ptr d)
		{
			return *static_cast<jpeg_state*>(d->client_data);
		}

		void source_noop(j_decompress_ptr)
		{
		}

		// The next block of a file that is not resident. At the end, an end-of-image marker returned as
		// a suspension, as Diffractor's decoder gave it: libjpeg stops at the rows it has, or takes the
		// marker on its next read and fills what is missing.
		boolean fill_input_buffer(const j_decompress_ptr d)
		{
			auto& s = state_of(d);

			if (s.file && s.next < s.file->size())
			{
				const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(s.block.size(), s.file->size() - s.next));
				s.file->read(s.next, s.block.data(), n);
				s.next += n;
				d->src->next_input_byte = s.block.data();
				d->src->bytes_in_buffer = n;
				return TRUE;
			}

			static const JOCTET end_of_image[] = {0xFF, JPEG_EOI};
			s.ended = true;
			d->src->next_input_byte = end_of_image;
			d->src->bytes_in_buffer = sizeof(end_of_image);
			return s.suspend_at_eof ? FALSE : TRUE;
		}

		void skip_input_data(const j_decompress_ptr d, const long num_bytes)
		{
			if (num_bytes <= 0) return;

			auto& s = state_of(d);
			const auto skip = static_cast<std::size_t>(num_bytes);
			const auto in_buffer = std::min(skip, d->src->bytes_in_buffer);
			d->src->next_input_byte += in_buffer;
			d->src->bytes_in_buffer -= in_buffer;

			// A skip past the block continues in the file.
			if (s.file && skip > in_buffer) s.next = std::min<std::uint64_t>(s.file->size(), s.next + (skip - in_buffer));
		}

		jpeg_state_ptr make_state()
		{
			jpeg_state_ptr s(new jpeg_state);
			s->dinfo.err = jpeg_std_error(&s->jerr);
			s->jerr.error_exit = error_exit;
			s->jerr.output_message = output_message;

			try
			{
				jpeg_create_decompress(&s->dinfo);
			}
			catch (...)
			{
				jpeg_destroy_decompress(&s->dinfo);
				throw;
			}

			s->created = true;
			s->dinfo.client_data = s.get();
			s->dinfo.src = &s->source;
			s->source.init_source = source_noop;
			s->source.fill_input_buffer = fill_input_buffer;
			s->source.skip_input_data = skip_input_data;
			s->source.resync_to_restart = jpeg_resync_to_restart;
			s->source.term_source = source_noop;

			// libjpeg discards APPn segments unless asked; APP1 holds the EXIF orientation.
			jpeg_save_markers(&s->dinfo, app1_marker, 0xFFFF);
			return s;
		}

		// NV12 has one chroma pair for each 2 by 2 luma block, so each chroma axis must be at full or half
		// resolution for a whole number of samples to average into it, and Cb and Cr must match, since
		// one set of factors indexes both. The raw reads take max_chunk rows at a time, and libjpeg
		// refuses one shorter than an iMCU row, so a vertical sampling factor above 2 decodes as BGRX.
		bool can_write_nv12(const jpeg_decompress_struct& d)
		{
			if (d.jpeg_color_space != JCS_YCbCr || d.num_components != 3 || d.data_precision != 8 || d.comp_info == nullptr)
			{
				return false;
			}

			const auto& luma = d.comp_info[0];
			const auto& cb = d.comp_info[1];
			const auto& cr = d.comp_info[2];

			if (cb.h_samp_factor != cr.h_samp_factor || cb.v_samp_factor != cr.v_samp_factor) return false;
			if (luma.v_samp_factor * DCTSIZE > static_cast<int>(max_chunk)) return false;

			return (luma.h_samp_factor == cb.h_samp_factor || luma.h_samp_factor == cb.h_samp_factor * 2) &&
				(luma.v_samp_factor == cb.v_samp_factor || luma.v_samp_factor == cb.v_samp_factor * 2);
		}

		// What one whole-image coefficient array holds for the stream whose header has been read: always
		// at full resolution, since a reduced decode reduces the transform's output, never the
		// coefficients it reads. Nothing for a lossless stream, whose data units are single samples.
		std::uint64_t coefficient_bytes(const jpeg_decompress_struct& d)
		{
			if (d.comp_info == nullptr || d.num_components <= 0 || d.block_size != DCTSIZE) return 0;

			// jdcoefct pads the array to a whole number of sampling blocks each way.
			const auto round_up = [](const std::uint64_t value, const std::uint64_t to)
			{
				return to > 0 ? (value + to - 1) / to * to : value;
			};

			std::uint64_t result = 0;

			for (auto ci = 0; ci < d.num_components; ++ci)
			{
				const auto& comp = d.comp_info[ci];
				result += round_up(comp.width_in_blocks, static_cast<std::uint64_t>(comp.h_samp_factor)) *
					round_up(comp.height_in_blocks, static_cast<std::uint64_t>(comp.v_samp_factor)) * sizeof(JBLOCK);
			}

			return result;
		}

		// True when the stream must be buffered whole before a row comes out: a progressive or
		// non-interleaved one. The test jdinput applies before jdmaster asks for whole-image arrays.
		bool buffers_whole_image(const jpeg_decompress_struct& d)
		{
			return d.progressive_mode || d.comps_in_scan < d.num_components;
		}

		// The TIFF stream of the last EXIF block libjpeg kept, or nothing.
		std::span<const std::uint8_t> last_exif_block(const jpeg_decompress_struct& d)
		{
			std::span<const std::uint8_t> block;

			for (const auto* m = d.marker_list; m != nullptr; m = m->next)
			{
				if (m->marker != app1_marker || m->data_length < 6 || std::memcmp(m->data, "Exif\0\0", 6) != 0) continue;
				block = {m->data + 6, m->data_length - 6};
			}

			return block;
		}

		void clear_rows(const pixel_buffer& out, const std::uint32_t first, const std::uint32_t end, const std::size_t row_bytes)
		{
			for (auto y = first; y < end; ++y) std::memset(out.pixels + out.stride * y, 0, row_bytes);
		}

		// B8G8R8X8 a chunk of rows at a time, the rows the data never reached cleared. The rows libjpeg
		// produced.
		std::uint32_t read_bgrx(jpeg_decompress_struct& d, const pixel_buffer& out, image_sink& sink)
		{
			auto y = 0u;
			const auto cy = d.output_height;
			const auto cx = d.output_width;
			const auto precision = d.data_precision;

			JSAMPROW rows[max_chunk];

			// 12- and 16-bit scans come out of their own entry points with a wider sample, so they are
			// read into scratch rows and scaled down into the 8-bit output.
			const auto wide = precision > 8;
			const auto wide_max = wide ? (1u << precision) - 1u : 1u;
			const auto components = static_cast<std::uint32_t>(d.out_color_components);
			const auto row_samples = cx * components;
			std::vector<std::uint16_t> scratch(wide ? static_cast<std::size_t>(row_samples) * max_chunk : 0u);
			std::uint16_t* wide_rows[max_chunk];

			while (y < cy)
			{
				if (sink.cancelled()) break;
				const auto chunk = std::min(max_chunk, cy - y);

				for (auto i = 0u; i < chunk; i++) rows[i] = out.pixels + out.stride * (y + i);

				auto read = 0u;

				if (wide)
				{
					for (auto i = 0u; i < chunk; i++) wide_rows[i] = scratch.data() + static_cast<std::size_t>(row_samples) * i;

					read = precision <= 12
						       ? jpeg12_read_scanlines(&d, reinterpret_cast<J12SAMPARRAY>(wide_rows), chunk)
						       : jpeg16_read_scanlines(&d, wide_rows, chunk);

					for (auto i = 0u; i < read; i++)
					{
						const auto* const src = wide_rows[i];
						auto* const dst = rows[i];

						if (components == 1)
						{
							for (auto x = 0u; x < cx; x++)
							{
								const auto v = static_cast<std::uint8_t>((src[x] * 255u + wide_max / 2u) / wide_max);
								dst[x * 4 + 0] = v;
								dst[x * 4 + 1] = v;
								dst[x * 4 + 2] = v;
								dst[x * 4 + 3] = 0xff;
							}
						}
						else
						{
							for (auto n = 0u; n < row_samples; n++)
							{
								dst[n] = static_cast<std::uint8_t>((src[n] * 255u + wide_max / 2u) / wide_max);
							}
						}
					}
				}
				else
				{
					read = jpeg_read_scanlines(&d, rows, chunk);

					// Grayscale arrives a byte a pixel and is spread in place, from the end.
					if (components == 1)
					{
						for (auto i = 0u; i < read; ++i)
						{
							auto* const row = rows[i];

							for (auto x = cx; x-- > 0;)
							{
								const auto value = row[x];
								row[x * 4 + 0] = value;
								row[x * 4 + 1] = value;
								row[x * 4 + 2] = value;
								row[x * 4 + 3] = 0xff;
							}
						}
					}
				}

				if (read == 0) break;

				if (d.out_color_space == JCS_CMYK)
				{
					// Adobe stores CMYK inverted, 255 meaning no ink; without its marker the values are
					// direct, and inverted here.
					const auto adobe = d.saw_Adobe_marker != 0;

					for (auto yy = 0u; yy < read; yy++)
					{
						auto* const row = rows[yy];

						for (auto x = 0u; x < cx; x++)
						{
							const auto xx = x * 4;
							const int c = adobe ? row[xx + 0] : 255 - row[xx + 0];
							const int m = adobe ? row[xx + 1] : 255 - row[xx + 1];
							const int ye = adobe ? row[xx + 2] : 255 - row[xx + 2];
							const int k = adobe ? row[xx + 3] : 255 - row[xx + 3];

							row[xx + bgrx_red] = static_cast<std::uint8_t>((c * k + 127) / 255);
							row[xx + bgrx_green] = static_cast<std::uint8_t>((m * k + 127) / 255);
							row[xx + bgrx_blue] = static_cast<std::uint8_t>((ye * k + 127) / 255);
							row[xx + 3] = 0;
						}
					}
				}

				y += read;
			}

			clear_rows(out, y, cy, std::size_t{4} * cx);
			return y;
		}

		// NV12 from libjpeg's raw planes: the luma rows as they are, the chroma averaged down to one
		// pair a 2 by 2 block where a plane holds more. The rows the data never reached are cleared.
		std::uint32_t read_nv12(jpeg_decompress_struct& d, const pixel_buffer& out, image_sink& sink)
		{
			const auto cx = d.output_width;
			const auto cy = d.output_height;

			const auto ucx = d.comp_info[1].downsampled_width;
			const auto ucy = d.comp_info[1].downsampled_height;

			// A 4:2:0 axis needs one sample an output pair; a full resolution one, two averaged.
			const auto taps_x = std::max(1u, 2 * ucx / cx);
			const auto taps_y = std::max(1u, 2 * ucy / cy);
			const auto taps = taps_x * taps_y;

			JSAMPROW py[max_chunk];
			JSAMPROW pu[max_chunk];
			JSAMPROW pv[max_chunk];

			// The raw path bypasses the upsampler, so libjpeg transforms whole blocks and the last one
			// overruns the component's width; the scratch rows carry that padding.
			const auto scratch_stride = (static_cast<std::size_t>(cx) + DCTSIZE + 15) & ~std::size_t{15};
			std::vector<std::uint8_t> scratch(scratch_stride * max_chunk * 3);

			const auto cy_div2 = cy & ~1u;
			const auto cx_div2 = cx & ~1u;

			JSAMPARRAY plane[3]{py, pu, pv};

			for (std::uint32_t i = 0; i < max_chunk; i++)
			{
				py[i] = scratch.data() + scratch_stride * i;
				pu[i] = scratch.data() + scratch_stride * (i + max_chunk);
				pv[i] = scratch.data() + scratch_stride * (i + max_chunk * 2);
			}

			auto y = 0u;

			while (y < cy_div2)
			{
				if (sink.cancelled()) break;
				const auto read = jpeg_read_raw_data(&d, plane, max_chunk);

				if (read == 0) break;

				for (auto yy = 0u; yy < read && y + yy < cy_div2; yy++)
				{
					std::memcpy(out.pixels + out.stride * (y + yy), py[yy], cx_div2);
				}

				// Each call returns one iMCU row: at 1:8, a source with no vertical subsampling gives a
				// single luma row and a single chroma row. A chroma pair starts on an even luma row, so a
				// call starting on an odd one adds none, and one holding fewer than taps_y chroma rows
				// averages the rows it has rather than reading scratch it did not fill.
				const auto chroma_lines = read * taps_y / 2;

				for (auto yy = y & 1u; yy < read && y + yy < cy_div2; yy += 2)
				{
					const auto src_row = ((y + yy) / 2) * taps_y - y * taps_y / 2;

					if (src_row >= chroma_lines) break;

					auto* const puvp = out.pixels + out.stride * (cy_div2 + (y + yy) / 2);

					if (taps == 1)
					{
						const auto* const pup = pu[src_row];
						const auto* const pvp = pv[src_row];

						for (auto xx = 0u; xx < cx_div2; xx += 2)
						{
							puvp[xx + 0] = pup[xx / 2];
							puvp[xx + 1] = pvp[xx / 2];
						}
					}
					else
					{
						const auto rows = std::min(taps_y, chroma_lines - src_row);
						const auto n = taps_x * rows;

						for (auto xx = 0u; xx < cx_div2; xx += 2)
						{
							const auto src_col = (xx / 2) * taps_x;
							auto u_sum = 0u;
							auto v_sum = 0u;

							for (auto sy = 0u; sy < rows; sy++)
							{
								const auto* const pup = pu[src_row + sy];
								const auto* const pvp = pv[src_row + sy];

								for (auto sx = 0u; sx < taps_x; sx++)
								{
									u_sum += pup[src_col + sx];
									v_sum += pvp[src_col + sx];
								}
							}

							puvp[xx + 0] = static_cast<std::uint8_t>((u_sum + n / 2) / n);
							puvp[xx + 1] = static_cast<std::uint8_t>((v_sum + n / 2) / n);
						}
					}
				}

				y += read;
			}

			// Only what libjpeg never wrote: the tail of the luma plane and the matching tail of the
			// chroma. Clearing all the chroma would leave Cb and Cr at 0, which renders green.
			const auto luma_rows = std::min(y, cy_div2);
			const auto chroma_rows = (luma_rows + 1) / 2;

			clear_rows(out, luma_rows, cy_div2, cx_div2);
			clear_rows(out, cy_div2 + chroma_rows, cy_div2 + cy_div2 / 2, cx_div2);
			return y;
		}

		// Finishes a decompression that produced every row, abandons one that did not; an error
		// finishing is no reason to discard rows already written.
		void close(jpeg_decompress_struct& d) noexcept
		{
			try
			{
				if (d.output_scanline < d.output_height) jpeg_abort_decompress(&d);
				else jpeg_finish_decompress(&d);
			}
			catch (const jpeg_failure&)
			{
				try
				{
					jpeg_abort_decompress(&d);
				}
				catch (const jpeg_failure&)
				{
				}
			}
		}
	}

	void decode_jpeg(reader& r, image_sink& sink, const decode_options& options, jpeg_state_ptr& state, decode_result& result)
	{
		if (!state) state = make_state();

		auto& s = *state;
		auto& d = s.dinfo;

		// The decompressor is reused for every image, so one abandoned part way, or left by an error,
		// must not leave parse state or markers for this one to resume into.
		jpeg_abort_decompress(&d);

		s.ended = false;
		s.next = 0;

		if (const auto resident = r.resident(); !resident.empty() || r.size() == 0)
		{
			s.file = nullptr;
			s.source.next_input_byte = resident.data();
			s.source.bytes_in_buffer = resident.size();
		}
		else
		{
			s.file = &r;
			s.block.resize(stream_block);
			s.source.next_input_byte = nullptr;
			s.source.bytes_in_buffer = 0;
		}

		// However the decode ends, the decompressor lets go of the reader, and of the image's memory.
		struct release
		{
			jpeg_state& s;

			~release()
			{
				s.file = nullptr;

				try
				{
					jpeg_abort_decompress(&s.dinfo);
				}
				catch (const jpeg_failure&)
				{
				}
			}
		} released{s};

		try
		{
			if (jpeg_read_header(&d, TRUE) != JPEG_HEADER_OK)
			{
				result.truncated = s.ended;
				result.error = "the JPEG's header cannot be read";
				return;
			}

			const auto exif = last_exif_block(d);

			frame_info frame;
			frame.source_width = d.image_width;
			frame.source_height = d.image_height;
			frame.orientation = summarize_exif(exif).orientation.value_or(1);
			frame.scale_denominator = scale_denominator_for(d.image_width, d.image_height, options.target_width, options.target_height);
			frame.working_bytes = buffers_whole_image(d) ? coefficient_bytes(d) : 0;

			const auto scale = frame.scale_denominator;
			const auto scaled_width = (d.image_width + scale - 1) / scale;
			const auto scaled_height = (d.image_height + scale - 1) / scale;
			const auto nv12 = options.allow_nv12 && can_write_nv12(d) && scaled_width >= 2 && scaled_height >= 2;

			if (d.jpeg_color_space == JCS_YCCK || d.jpeg_color_space == JCS_CMYK)
			{
				d.out_color_space = JCS_CMYK;
				d.raw_data_out = FALSE;
			}
			else if (d.jpeg_color_space == JCS_GRAYSCALE)
			{
				// Lossless grayscale permits no colour conversion, even at eight bits.
				d.out_color_space = JCS_GRAYSCALE;
				d.raw_data_out = FALSE;
			}
			else
			{
				d.out_color_space = nv12 ? JCS_YCbCr : JCS_EXT_BGRX;
				d.raw_data_out = nv12 ? TRUE : FALSE;
			}

			// Box-filtered chroma shows as blocky colour on saturated edges, so the caller decides, not
			// the scale. NV12 bypasses the upsampler: whoever samples it resolves its chroma.
			d.do_fancy_upsampling = !nv12 && options.smooth_chroma ? TRUE : FALSE;
			d.do_block_smoothing = FALSE;
			d.dct_method = JDCT_ISLOW;

			// Powers of two only: NV12's scratch rows rely on the scaled block size being one.
			d.scale_num = 1;
			d.scale_denom = scale;

			jpeg_calc_output_dimensions(&d);

			frame.format = nv12 ? pixel_format::nv12 : pixel_format::bgrx8;
			frame.width = nv12 ? d.output_width & ~1u : d.output_width;
			frame.height = nv12 ? d.output_height & ~1u : d.output_height;
			result.frame = frame;

			if (frame.width == 0 || frame.height == 0)
			{
				result.error = "the JPEG's image is empty";
				return;
			}

			// The block lies in libjpeg's memory, which the decode lets go of, so only the sink sees it.
			frame.exif = exif;
			const auto out = sink.allocate(frame);

			if (!out.pixels)
			{
				result.refused = true;
				return;
			}

			if (!jpeg_start_decompress(&d))
			{
				result.truncated = s.ended;
				result.error = "the JPEG's data ends before its first row";
				return;
			}

			const auto rows = nv12 ? read_nv12(d, out, sink) : read_bgrx(d, out, sink);
			close(d);

			result.truncated = s.ended;
			result.cancelled = sink.cancelled();
			result.success = rows > 0 && !result.cancelled;
			if (!result.success && !result.cancelled) result.error = "the JPEG's data ends before its first row";
		}
		catch (const jpeg_failure& e)
		{
			const auto frame = result.frame;
			result = {};
			result.frame = frame;
			result.truncated = s.ended;
			result.error = e.what();
		}
	}

	struct native_jpeg_buffers
	{
		std::vector<std::uint8_t> pixels, row, photoshop, profile;
		std::map<unsigned, std::span<const std::uint8_t>> icc;
	};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611) // all owning objects live outside the setjmp frame
#endif
	static void read_document_jpeg(document_decode& decode, jpeg_state& s, native_jpeg_buffers& buffers)
	{
		auto& d = s.dinfo;
		auto& pixels = buffers.pixels;
		auto& row = buffers.row;
		auto& photoshop = buffers.photoshop;
		auto& profile = buffers.profile;
		auto& icc = buffers.icc;
		const auto resident = decode.input.resident();
		if (!resident.empty())
		{
			s.source.next_input_byte = resident.data();
			s.source.bytes_in_buffer = resident.size();
		}
		else
		{
			s.file = &decode.input;
			s.block.resize(stream_block);
		}
		s.suspend_at_eof = false;
		s.warnings = &decode.result.warnings;
		s.jerr.output_message = [](j_common_ptr info)
		{
			auto& state = *static_cast<jpeg_state*>(info->client_data);
			char message[JMSG_LENGTH_MAX]{};
			info->err->format_message(info, message);
			state.warnings->emplace_back(message);
		};
		std::jmp_buf jump;
		s.jump = &jump;
		s.jerr.error_exit = [](j_common_ptr info)
		{
			auto& state = *static_cast<jpeg_state*>(info->client_data);
			info->err->format_message(info, state.message);
			std::longjmp(*state.jump, 1);
		};
		if (setjmp(jump))
		{
			decode.result.truncated = s.ended;
			decode.fail(s.message);
		}
		{
			for (int marker = 0; marker < 16; ++marker) jpeg_save_markers(&d, JPEG_APP0 + marker, 0);
			jpeg_save_markers(&d, JPEG_COM, 0);
			for (const int marker : {1, 2, 13}) jpeg_save_markers(&d, JPEG_APP0 + marker, 0xffff);
			if (jpeg_read_header(&d, TRUE) != JPEG_HEADER_OK ||
				d.image_width > 32767 || d.image_height > 32767)
				decode.fail("invalid JPEG dimensions or header");
			const auto thumbnail = decode.options.thumbnail_width && decode.options.thumbnail_height;
			d.scale_denom = 1;
			if (thumbnail)
			{
				for (const auto divisor : {8u, 4u, 2u})
				{
					if (d.image_width > static_cast<std::uint64_t>(decode.options.thumbnail_width) * divisor &&
						d.image_height > static_cast<std::uint64_t>(decode.options.thumbnail_height) * divisor)
					{
						d.scale_denom = divisor;
						break;
					}
				}
			}
			d.dither_mode = JDITHER_NONE;
			d.two_pass_quantize = FALSE;
			d.quantize_colors = FALSE;
			d.out_color_space = d.jpeg_color_space == JCS_GRAYSCALE ? JCS_GRAYSCALE :
				(d.jpeg_color_space == JCS_CMYK || d.jpeg_color_space == JCS_YCCK) ? JCS_CMYK : JCS_RGB;
			d.dct_method = thumbnail ? JDCT_FASTEST : JDCT_FLOAT;
			d.do_fancy_upsampling = thumbnail ? FALSE : TRUE;
			d.do_block_smoothing = thumbnail ? FALSE : TRUE;
			decode.working_bytes(buffers_whole_image(d) ? coefficient_bytes(d) : 0);
			if (!jpeg_start_decompress(&d)) decode.fail("the JPEG decoder did not start");

			constexpr char xmp_id[] = "http://ns.adobe.com/xap/1.0/";
			constexpr char photoshop_id[] = "Photoshop 3.0";
			constexpr char icc_id[] = "ICC_PROFILE";
			std::span<const std::uint8_t> xmp;
			for (auto* marker = d.marker_list; marker; marker = marker->next)
			{
				const std::span<const std::uint8_t> bytes(marker->data, marker->data_length);
				if (marker->marker == JPEG_APP0 + 1)
				{
					if (bytes.size() > sizeof(xmp_id) && std::memcmp(bytes.data(), xmp_id, sizeof(xmp_id)) == 0)
						xmp = bytes.subspan(sizeof(xmp_id));
					else if (bytes.size() > 6 && std::memcmp(bytes.data(), "Exif\0\0", 6) == 0)
						decode.output.metadata(document_metadata::exif, bytes);
				}
				else if (marker->marker == JPEG_APP0 + 13 && bytes.size() > 24 &&
					std::memcmp(bytes.data(), photoshop_id, sizeof(photoshop_id)) == 0)
				{
					if (photoshop.empty()) photoshop.insert(photoshop.end(), bytes.begin(), bytes.begin() + sizeof(photoshop_id));
					photoshop.insert(photoshop.end(), bytes.begin() + sizeof(photoshop_id), bytes.end());
					decode.working_bytes(photoshop.size());
				}
				else if (marker->marker == JPEG_APP0 + 2 && bytes.size() > sizeof(icc_id) + 2 &&
					std::memcmp(bytes.data(), icc_id, sizeof(icc_id)) == 0)
				{
					icc[bytes[sizeof(icc_id)]] = bytes.subspan(sizeof(icc_id) + 2);
				}
			}
			for (const auto& [sequence, bytes] : icc)
			{
				(void)sequence;
				decode.working_bytes(profile.size() + bytes.size());
				profile.insert(profile.end(), bytes.begin(), bytes.end());
			}
			if (!profile.empty()) decode.output.metadata(document_metadata::icc, profile);
			if (!photoshop.empty()) decode.output.metadata(document_metadata::iptc, photoshop);
			if (!xmp.empty()) decode.output.metadata(document_metadata::xmp, xmp);
			decode.output.metadata_complete();

			auto& info = decode.result.info;
			info.width = d.image_width;
			info.height = d.image_height;
			info.storage = d.output_components == 1 ? document_storage::gray8 : document_storage::bgr24;
			info.bits_per_pixel = storage_bits(info.storage);
			if (d.saw_JFIF_marker) info.version = static_cast<std::uint16_t>((d.JFIF_major_version << 8) | d.JFIF_minor_version);
			info.has_resolution = d.density_unit == 1 || d.density_unit == 2;
			if (d.density_unit == 1)
			{
				info.pixels_per_meter_x = d.X_density * 3937u / 100;
				info.pixels_per_meter_y = d.Y_density * 3937u / 100;
			}
			else if (d.density_unit == 2)
			{
				info.pixels_per_meter_x = d.X_density * 100u;
				info.pixels_per_meter_y = d.Y_density * 100u;
			}
			if (!decode.options.pixels)
			{
				jpeg_abort_decompress(&d);
				return;
			}
			document_frame frame;
			frame.width = d.output_width;
			frame.height = d.output_height;
			frame.storage = info.storage;
			decode.begin(frame);
			const auto line_bytes = static_cast<std::size_t>(d.output_components) * d.output_width;
			const auto stride = document_row_bytes(frame.width, frame.storage);
			decode.working_bytes(line_bytes * 4 + std::max(stride, line_bytes));
			pixels.resize(line_bytes * 4);
			row.resize(std::max(stride, line_bytes), 0);
			JSAMPROW lines[] = {pixels.data(), pixels.data() + line_bytes,
				pixels.data() + line_bytes * 2, pixels.data() + line_bytes * 3};
			const auto width = d.output_width;
			const auto color_space = d.out_color_space;
			const bool inverted = d.saw_Adobe_marker != 0;
			while (d.output_scanline < d.output_height)
			{
				const auto first = d.output_scanline;
				const auto count = jpeg_read_scanlines(&d, lines, 4);
				if (!count) decode.fail("the JPEG decoder produced no complete row");
				for (unsigned y = 0; y < count; ++y)
				{
					const auto* source = lines[y];
					if (color_space == JCS_GRAYSCALE)
						std::memcpy(row.data(), source, line_bytes);
					else if (color_space == JCS_CMYK)
					{
						for (unsigned x = 0; x < width; ++x)
						{
							const auto black = inverted ? source[x * 4 + 3] : 255u - source[x * 4 + 3];
							for (unsigned color = 0; color < 3; ++color)
							{
								const auto ink = source[x * 4 + 2 - color];
								row[x * 3 + color] = static_cast<std::uint8_t>((inverted ? ink : 255u - ink) * black / 255);
							}
						}
					}
					else
					{
						for (unsigned x = 0; x < width; ++x)
						{
							row[x * 3] = source[x * 3 + 2];
							row[x * 3 + 1] = source[x * 3 + 1];
							row[x * 3 + 2] = source[x * 3];
						}
					}
					decode.row(first + y, row);
				}
				decode.output.progress(d.output_scanline, d.output_height);
			}
			decode.end();
			jpeg_finish_decompress(&d);
		}
	}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

	void document_jpeg(document_decode& decode)
	{
		auto state = make_state();
		native_jpeg_buffers buffers;
		read_document_jpeg(decode, *state, buffers);
	}
}
