// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Replaying an update patched into a live file as an interruption could leave it. Writes
// between two flushes may reach the disk in any order, so every state is each flushed step whole and
// any subset of the next.

#pragma once

#include "libfiles/stream.h"
#include "libfiles/write.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace test_build
{
	// A live file, recording each write and the flushes before it.
	class recording_stream final : public libfiles::writable_stream
	{
	public:
		struct write_record
		{
			std::uint64_t offset = 0;
			std::vector<std::uint8_t> data;
			int flushes_before = 0;
		};

		explicit recording_stream(std::vector<std::uint8_t> file) : _file(std::move(file))
		{
		}

		std::uint64_t size() override { return _file.size(); }

		std::size_t read(const std::uint64_t offset, void* data, const std::size_t size) override
		{
			if (offset >= _file.size()) return 0;
			const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(size, _file.size() - offset));
			std::memcpy(data, _file.data() + offset, n);
			return n;
		}

		bool write(const std::uint64_t offset, const void* data, const std::size_t size) override
		{
			const auto* const p = static_cast<const std::uint8_t*>(data);
			writes.push_back({offset, std::vector<std::uint8_t>(p, p + size), flushes});
			apply(_file, writes.back());
			return true;
		}

		bool resize(const std::uint64_t size) override
		{
			_file.resize(static_cast<std::size_t>(size));
			return true;
		}

		bool flush() override
		{
			++flushes;
			return true;
		}

		static void apply(std::vector<std::uint8_t>& file, const write_record& w)
		{
			if (file.size() < w.offset + w.data.size()) file.resize(static_cast<std::size_t>(w.offset + w.data.size()));
			std::ranges::copy(w.data, file.begin() + static_cast<std::ptrdiff_t>(w.offset));
		}

		std::vector<write_record> writes;
		int flushes = 0;

	private:
		std::vector<std::uint8_t> _file;
	};

	// Every file an interruption could leave; empty when a step holds too many writes to try.
	inline std::vector<std::vector<std::uint8_t>> interrupted_states(const std::vector<std::uint8_t>& original,
	                                                                 const std::vector<recording_stream::write_record>& writes)
	{
		std::vector<std::vector<std::uint8_t>> states;
		auto settled = original;
		std::size_t i = 0;

		while (i < writes.size())
		{
			auto j = i;
			while (j < writes.size() && writes[j].flushes_before == writes[i].flushes_before) ++j;

			const auto n = j - i;
			if (n > 12) return {};

			for (std::uint32_t landed = 0; landed < (1u << n); ++landed)
			{
				auto state = settled;
				for (std::size_t k = 0; k < n; ++k) if (landed & (1u << k)) recording_stream::apply(state, writes[i + k]);
				states.push_back(std::move(state));
			}

			for (auto k = i; k < j; ++k) recording_stream::apply(settled, writes[k]);
			i = j;
		}

		states.push_back(std::move(settled));
		return states;
	}

	// Every file an interruption could leave when the plan is patched into original where it lies, the
	// finished one last; empty when the plan is not a patch.
	inline std::vector<std::vector<std::uint8_t>> patch_states(const std::vector<std::uint8_t>& original,
	                                                           const libfiles::write_plan& plan)
	{
		if (!plan.patchable()) return {};

		recording_stream live(original);
		libfiles::write_in_place(live, plan);
		return interrupted_states(original, live.writes);
	}
}
