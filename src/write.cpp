// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: The update framework: dispatch to each format's planner, validation of the plan it
// returns, patching a file in place, and the one-pass rewrite.

#include "write_internal.h"

#include <algorithm>
#include <exception>
#include <limits>

namespace libfiles
{
	bool write_plan::in_place() const noexcept
	{
		return ok() && std::ranges::all_of(replacements, [](const replacement& r)
		{
			return r.length == r.data.size();
		});
	}

	bool write_plan::patchable() const noexcept
	{
		return ok() && std::ranges::all_of(replacements, [this](const replacement& r)
		{
			return r.length == r.data.size() || (r.length == 0 && r.offset == source_size);
		});
	}

	std::uint64_t write_plan::result_size() const noexcept
	{
		auto size = source_size;
		for (const auto& r : replacements) size = size - r.length + r.data.size();
		return size;
	}

	file_format detect_file_format(const std::span<const std::uint8_t> head) noexcept
	{
		switch (detect_image_format(head))
		{
		case image_format::jpeg: return file_format::jpeg;
		case image_format::png: return file_format::png;
		case image_format::gif: return file_format::gif;
		case image_format::webp: return file_format::webp;
		case image_format::tiff: return file_format::tiff;
		case image_format::psd: return file_format::psd;
		case image_format::heif: return file_format::unknown;
		default: break;
		}

		if (head.size() >= 16 && detail::is_asf(head)) return file_format::asf;
		if (head.size() >= 12 && detail::is_riff_media(head)) return file_format::riff;
		if (head.size() >= 9 && detail::is_flv(head)) return file_format::flv;
		if (head.size() >= 8 && detail::is_iso_media(head)) return file_format::mp4;
		if (head.size() >= 4 && detail::is_mpeg_audio(head)) return file_format::mp3;
		return file_format::unknown;
	}

	bool can_update(const file_format format) noexcept
	{
		return format != file_format::unknown;
	}

	packet_reading read_packet(reader& source)
	{
		packet_reading result;
		if (source.size() < 16) return result;

		try
		{
			// One identification for the scans, read_packet and plan_update, so they agree on what a file is.
			switch (const auto type = identify(source))
			{
			case file_type::heif:
			case file_type::jxl:
				{
					// HEIF and JPEG XL cannot be updated, but their packets read.
					std::vector<std::uint8_t> packet;
					result.readable = type == file_type::heif ? detail::heif_packet(source, packet) : detail::jxl_packet(source, packet);
					if (result.readable) result.packet.assign(packet.begin(), packet.end());
				}
				break;
			case file_type::mp4:
				detail::read_mp4_packet(source, result);
				break;
			case file_type::asf:
				detail::read_asf_packet(source, result);
				break;
			case file_type::avi:
			case file_type::wav:
				detail::read_riff_packet(source, result);
				break;
			case file_type::mpeg_audio:
			case file_type::flac:
				detail::read_mp3_packet(source, result);
				break;
			case file_type::flv:
				detail::read_flv_packet(source, result);
				break;
			case file_type::aiff:
				detail::read_aiff_packet(source, result);
				break;
			case file_type::jpeg:
			case file_type::png:
			case file_type::gif:
			case file_type::webp:
			case file_type::tiff:
			case file_type::psd:
				{
					const auto scanned = scan(source);
					result.readable = scanned.success;
					result.packet.assign(scanned.metadata.xmp.begin(), scanned.metadata.xmp.end());
				}
				break;
			default:
				break;
			}
		}
		catch (const std::exception&)
		{
			result = {};
		}

		return result;
	}

	write_plan plan_update(reader& source, const metadata_update& update)
	{
		write_plan plan;
		plan.source_size = source.size();

		if (source.size() < 16)
		{
			plan.error = "the file is too short to update";
			return plan;
		}

		try
		{
			switch (info(identify(source)).update_format)
			{
			case file_format::jpeg: detail::plan_jpeg(source, update, plan);
				break;
			case file_format::png: detail::plan_png(source, update, plan);
				break;
			case file_format::gif: detail::plan_gif(source, update, plan);
				break;
			case file_format::webp: detail::plan_webp(source, update, plan);
				break;
			case file_format::tiff: detail::plan_tiff(source, update, plan);
				break;
			case file_format::psd: detail::plan_psd(source, update, plan);
				break;
			case file_format::mp4: detail::plan_mp4(source, update, plan);
				break;
			case file_format::asf: detail::plan_asf(source, update, plan);
				break;
			case file_format::riff: detail::plan_riff(source, update, plan);
				break;
			case file_format::mp3: detail::plan_mp3(source, update, plan);
				break;
			case file_format::flv: detail::plan_flv(source, update, plan);
				break;
			default:
				plan.error = "this file format cannot be updated";
				return plan;
			}

			// Insertions come before a replacement at the same offset, and keep the order their
			// planner gave them.
			std::ranges::stable_sort(plan.replacements, [](const replacement& a, const replacement& b)
			{
				return a.offset != b.offset ? a.offset < b.offset : (a.length == 0 && b.length != 0);
			});

			std::uint64_t end = 0;

			for (const auto& r : plan.replacements)
			{
				if (r.offset < end || !source.contains(r.offset, r.length))
				{
					throw std::logic_error("the planned changes overlap or leave the file");
				}

				end = r.offset + r.length;
			}
		}
		catch (const std::exception& e)
		{
			plan.replacements.clear();
			plan.error = e.what();
		}

		return plan;
	}

	void write_in_place(writable_stream& file, const write_plan& plan)
	{
		if (!plan.patchable()) throw write_error("the plan moves what the file holds");
		if (file.size() != plan.source_size) throw write_error("the file changed after the update was planned");

		const auto appended = [&plan](const replacement& r) { return r.length == 0 && r.offset == plan.source_size; };

		// What is added at the end goes first: until a patch refers to it, it is only bytes after
		// the end of everything the file describes.
		auto end = plan.source_size;

		for (const auto& r : plan.replacements)
		{
			if (!appended(r) || r.data.empty()) continue;
			if (!file.write(end, r.data.data(), r.data.size())) throw write_error("the file could not be written");
			end += r.data.size();
		}

		if (end != plan.source_size && !file.flush()) throw write_error("the file could not be flushed");

		std::vector<std::uint8_t> steps;
		for (const auto& r : plan.replacements) if (!appended(r)) steps.push_back(r.step);
		std::ranges::sort(steps);
		steps.erase(std::ranges::unique(steps).begin(), steps.end());

		for (const auto step : steps)
		{
			for (const auto& r : plan.replacements)
			{
				if (appended(r) || r.step != step || r.data.empty()) continue;
				if (!file.write(r.offset, r.data.data(), r.data.size())) throw write_error("the file could not be written");
			}

			if (!file.flush()) throw write_error("the file could not be flushed");
		}

		if (steps.empty() && end == plan.source_size && !file.flush()) throw write_error("the file could not be flushed");
	}

	void write_updated(reader& source, const write_plan& plan, writable_stream& destination)
	{
		if (!plan.ok()) throw write_error(plan.error);
		if (source.size() != plan.source_size) throw write_error("the file changed after the update was planned");

		// Larger than the reader's window, so the copies bypass it rather than going through it.
		std::vector<std::uint8_t> buffer(1024 * 1024);
		std::uint64_t in = 0;
		std::uint64_t out = 0;

		const auto put = [&destination, &out](const std::uint8_t* data, const std::size_t size)
		{
			if (size != 0 && !destination.write(out, data, size)) throw write_error("the file could not be written");
			out += size;
		};

		const auto copy_to = [&](const std::uint64_t end)
		{
			while (in < end)
			{
				const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), end - in));
				source.read(in, buffer.data(), n);
				put(buffer.data(), n);
				in += n;
			}
		};

		for (const auto& r : plan.replacements)
		{
			copy_to(r.offset);
			put(r.data.data(), r.data.size());
			in = r.offset + r.length;
		}

		copy_to(plan.source_size);

		if (!destination.resize(out)) throw write_error("the file could not be cut to size");
		if (!destination.flush()) throw write_error("the file could not be flushed");
	}
}

namespace libfiles::detail
{
	std::vector<replacement> diff_patches(const std::uint64_t offset, const std::span<const std::uint8_t> original,
	                                      const std::span<const std::uint8_t> updated)
	{
		// Unchanged runs shorter than this are written over rather than splitting the patch.
		constexpr std::size_t join = 16;

		std::vector<replacement> out;
		const auto n = std::min(original.size(), updated.size());
		std::size_t i = 0;

		while (i < n)
		{
			if (original[i] == updated[i])
			{
				++i;
				continue;
			}

			const auto start = i;
			auto end = i + 1;

			for (auto j = i + 1; j < n && j - end < join; ++j)
			{
				if (original[j] != updated[j]) end = j + 1;
			}

			out.push_back({offset + start, end - start, std::vector<std::uint8_t>(updated.begin() + static_cast<std::ptrdiff_t>(start),
			                                                                       updated.begin() + static_cast<std::ptrdiff_t>(end))});
			i = end;
		}

		return out;
	}

	std::optional<replacement> changed_span(const std::uint64_t offset, const std::span<const std::uint8_t> original,
	                                        const std::span<const std::uint8_t> updated, const std::uint8_t step)
	{
		const auto first = static_cast<std::size_t>(std::ranges::mismatch(original, updated).in1 - original.begin());
		if (first == original.size()) return {};

		auto last = original.size();
		while (last > first && original[last - 1] == updated[last - 1]) --last;

		return replacement{offset + first, last - first, std::vector<std::uint8_t>(updated.begin() + static_cast<std::ptrdiff_t>(first),
		                                                                            updated.begin() + static_cast<std::ptrdiff_t>(last)), step};
	}

	void push_changed(reader& r, write_plan& plan, const std::uint64_t offset, const std::span<const std::uint8_t> updated,
	                  const std::uint8_t step)
	{
		const auto original = r.read(offset, updated.size());
		if (auto p = changed_span(offset, original, updated, step)) plan.replacements.push_back(std::move(*p));
	}

	std::string serialize_packet(const xmp::meta& meta, const std::uint64_t old_length)
	{
		if (old_length > 0 && old_length <= std::numeric_limits<std::uint32_t>::max())
		{
			try
			{
				xmp::serialize_options exact;
				exact.use_compact_format = true;
				exact.exact_packet_length = true;
				exact.padding = static_cast<std::uint32_t>(old_length);
				auto packet = meta.serialize(exact);
				if (packet.size() == old_length) return packet;
			}
			catch (const xmp::error&)
			{
				// It does not fit where the old packet was; it is written out of place.
			}
		}

		xmp::serialize_options options;
		options.use_compact_format = true;
		return meta.serialize(options);
	}
}
