// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: Runs every registered test, or those whose name contains the first argument, and exits
// non-zero when any check fails.

#include "test.h"

#include <exception>

int main(const int argc, char** argv)
{
	const std::string_view filter = argc > 1 ? argv[1] : "";
	auto run = 0;
	auto failed = 0;

	for (const auto& t : test::registry())
	{
		if (!filter.empty() && t.name.find(filter) == std::string::npos) continue;

		auto& s = test::current();
		s.current_failed = false;
		++run;

		try
		{
			t.body();
		}
		catch (const std::exception& e)
		{
			test::fail(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
		}
		catch (...)
		{
			test::fail(__FILE__, __LINE__, "unexpected exception");
		}

		if (s.current_failed)
		{
			++failed;
			std::printf("FAIL  %s\n", t.name.c_str());
		}
		else
		{
			std::printf("PASS  %s\n", t.name.c_str());
		}
	}

	std::printf("\n%d tests, %d failed, %d checks, %d failed checks\n", run, failed, test::current().checks,
	            test::current().failures);

	if (run == 0)
	{
		std::printf("No test matched the filter.\n");
		return 2;
	}

	return failed == 0 ? 0 : 1;
}
