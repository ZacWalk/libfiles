// This file is part of libfiles.
// Copyright 2026  Zac Walker
// SPDX-License-Identifier: LGPL-2.1-or-later

// Purpose: A minimal test harness: named tests registered at static initialization, checks that
// record failures without stopping the test, and a runner that filters by name.

#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace test
{
	struct registration
	{
		std::string name;
		std::function<void()> body;
	};

	inline std::vector<registration>& registry()
	{
		static std::vector<registration> tests;
		return tests;
	}

	struct state
	{
		int checks = 0;
		int failures = 0;
		bool current_failed = false;
	};

	inline state& current()
	{
		static state s;
		return s;
	}

	struct registrar
	{
		registrar(const char* name, std::function<void()> body)
		{
			registry().push_back({name, std::move(body)});
		}
	};

	inline void fail(const char* file, const int line, const std::string& message)
	{
		auto& s = current();
		++s.failures;
		s.current_failed = true;
		std::printf("  FAIL %s:%d: %s\n", file, line, message.c_str());
	}

	inline void check(const bool ok, const char* expr, const char* file, const int line)
	{
		++current().checks;
		if (!ok) fail(file, line, expr);
	}

	inline void check_equal(const std::string_view actual, const std::string_view expected, const char* expr,
	                        const char* file, const int line)
	{
		++current().checks;
		if (actual != expected)
		{
			fail(file, line, std::string(expr) + "\n    expected: '" + std::string(expected) + "'\n    actual:   '" +
			     std::string(actual) + "'");
		}
	}

	template <typename A, typename B>
	void check_equal_value(const A& actual, const B& expected, const char* expr, const char* file, const int line)
	{
		++current().checks;
		if (!(actual == expected))
		{
			fail(file, line, std::string(expr) + " (expected " + std::to_string(expected) + ", actual " +
			     std::to_string(actual) + ")");
		}
	}
}

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)

#define TEST(name) \
	static void TEST_CONCAT(test_body_, __LINE__)(); \
	static const test::registrar TEST_CONCAT(test_registrar_, __LINE__)(name, TEST_CONCAT(test_body_, __LINE__)); \
	static void TEST_CONCAT(test_body_, __LINE__)()

#define CHECK(expr) test::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_STR(actual, expected) test::check_equal((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) test::check_equal_value((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
#define CHECK_THROWS(expr) \
	do \
	{ \
		auto threw_ = false; \
		try \
		{ \
			(void)(expr); \
		} \
		catch (...) \
		{ \
			threw_ = true; \
		} \
		test::check(threw_, "throws: " #expr, __FILE__, __LINE__); \
	} \
	while (false)
