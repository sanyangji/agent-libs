//
// Created by Luca Marturana on 31/03/15.
//

#include <gtest.h>
#include "sys_call_test.h"
#include "statsite_proxy.h"

/* statsite output:

counts.mycounter#xxx,yy|42.000000|1427796784
timers.myhist#we,ff.sum|31890.000000|1427796784
timers.myhist#we,ff.sum_sq|19643496.000000|1427796784
timers.myhist#we,ff.mean|475.970149|1427796784
timers.myhist#we,ff.lower|6.000000|1427796784
timers.myhist#we,ff.upper|894.000000|1427796784
timers.myhist#we,ff.count|67|1427796784
timers.myhist#we,ff.stdev|260.093455|1427796784
timers.myhist#we,ff.median|479.000000|1427796784
timers.myhist#we,ff.p50|479.000000|1427796784
timers.myhist#we,ff.p95|888.000000|1427796784
timers.myhist#we,ff.p99|894.000000|1427796784
timers.myhist#we,ff.rate|31890.000000|1427796784
timers.myhist#we,ff.sample_rate|67.000000|1427796784
timers.mytime.sum|6681.000000|1427796784
timers.mytime.sum_sq|853697.000000|1427796784
timers.mytime.mean|99.716418|1427796784
timers.mytime.lower|0.000000|1427796784
timers.mytime.upper|197.000000|1427796784
timers.mytime.count|67|1427796784
timers.mytime.stdev|53.298987|1427796784
timers.mytime.median|106.000000|1427796784
timers.mytime.p50|106.000000|1427796784
timers.mytime.p95|190.000000|1427796784
timers.mytime.p99|197.000000|1427796784
timers.mytime.rate|6681.000000|1427796784
timers.mytime.sample_rate|67.000000|1427796784
gauges.mygauge|2.000000|1427796784
sets.myset|53|1427796784

*/
TEST(statsd_metric, parse_counter)
{
	auto metric = statsd_metric();
	metric.parse_line("counts.mycounter#xxx,yy|42.000000|1427796784\n");
	EXPECT_EQ(1427796784U, metric.timestamp());
	EXPECT_EQ("mycounter", metric.name());
	EXPECT_EQ(statsd_metric::type_t::COUNT, metric.type());
	EXPECT_DOUBLE_EQ(42.0, metric.value());
	EXPECT_TRUE(metric.tags().find("xxx") != metric.tags().end());
	EXPECT_FALSE(metric.parse_line("counts.mycounter#xxx,yy|42.000000|1427796785\n"));

	metric = statsd_metric();
	metric.parse_line("counts.mycounter|42.000000|1427796784\n");
	EXPECT_EQ(1427796784U, metric.timestamp());
	EXPECT_EQ("mycounter", metric.name());
	EXPECT_DOUBLE_EQ(42.0, metric.value());
	EXPECT_EQ(statsd_metric::type_t::COUNT, metric.type());

	metric = statsd_metric();
	metric.parse_line("counts.mycounter.amazing|42.000000|1427796784\n");
	EXPECT_EQ(1427796784U, metric.timestamp());
	EXPECT_EQ("mycounter.amazing", metric.name());
}

TEST(statsd_metric, parser_histogram)
{
	auto metric = statsd_metric();
	metric.parse_line("timers.mytime.sum|6681.000000|1427796784\n");
	EXPECT_EQ(1427796784U, metric.timestamp());
	EXPECT_EQ("mytime", metric.name());
	EXPECT_DOUBLE_EQ(6681.0, metric.sum());
	EXPECT_EQ("", metric.container_id());
	EXPECT_FALSE(metric.parse_line("timers.mytime#we,ff.sum|6681.000000|1427796784\n"));

	metric = statsd_metric();
	metric.parse_line("timers.mytime#we,ff.sum|6681.000000|1427796784\n");
	EXPECT_EQ("mytime", metric.name());
	EXPECT_DOUBLE_EQ(6681.0, metric.sum());
	EXPECT_EQ("", metric.container_id());

	metric = statsd_metric();
	metric.parse_line("timers.lksajdlkjsal$mytime#we=ff.sum|6681.000000|1427796784\n");
	EXPECT_EQ("mytime", metric.name());
	EXPECT_DOUBLE_EQ(6681.0, metric.sum());
	EXPECT_EQ("ff", metric.tags().at("we"));
	EXPECT_EQ("lksajdlkjsal", metric.container_id());

	metric = statsd_metric();
	metric.parse_line("timers.mytime.p60|12.500000|1491419454\n");
	EXPECT_EQ("mytime", metric.name());
	double pct = 0.;
	EXPECT_FALSE(metric.percentile(50, pct));
	EXPECT_DOUBLE_EQ(0., pct);
	EXPECT_TRUE(metric.percentile(60, pct));
	EXPECT_DOUBLE_EQ(12.5, pct);
	EXPECT_EQ("", metric.container_id());
}

TEST(statsd_metric, parser_gauge)
{
	auto metric = statsd_metric();
	metric.parse_line("gauges.mygauge|2.000000|1427796784\n");
	EXPECT_EQ(1427796784U, metric.timestamp());
	EXPECT_EQ("mygauge", metric.name());
	EXPECT_DOUBLE_EQ(2.0, metric.value());
	EXPECT_EQ("", metric.container_id());
}

TEST(statsd_metric, parser_edge_cases)
{
	auto metric = statsd_metric();
	metric.parse_line("gauges.mygauge#|2.000000|1427796784\n");
	EXPECT_EQ(1427796784U, metric.timestamp());
	EXPECT_EQ("mygauge", metric.name());
	EXPECT_DOUBLE_EQ(2.0, metric.value());

	metric = statsd_metric();
	metric.parse_line("gauges.#|2.000000|1427796784\n");
	EXPECT_EQ(1427796784U, metric.timestamp());
	EXPECT_EQ("", metric.name());
	EXPECT_DOUBLE_EQ(2.0, metric.value());
}

TEST(statsd_metric, sanitize_container)
{
	EXPECT_EQ("d19fefe08-18fb-40e2-82c6-dd5d971785d0+statsd-example",
				statsd_metric::sanitize_container_id("d19fefe08-18fb-40e2-82c6-dd5d971785d0:statsd-example"));
	EXPECT_EQ("d19fefe08-18fb-40e2-82c6-dd5d971785d0:statsd-example",
				statsd_metric::desanitize_container_id("d19fefe08-18fb-40e2-82c6-dd5d971785d0+statsd-example"));
}

TEST(statsite_proxy, parser)
{
	auto output_file = fopen("resources/statsite_output.txt", "r");
	auto input_fd = fopen("/dev/null", "w");
	ASSERT_TRUE(output_file != NULL);

	// 300 chosen so to be bigger than anything in the above file.
	statsite_proxy proxy(make_pair(input_fd, output_file), false);

	auto ret = proxy.read_metrics();
	EXPECT_EQ(2U, ret.size());
	EXPECT_EQ(10U, std::get<0>(ret.at("")).size());
	EXPECT_EQ(10U, std::get<0>(ret.at("3ce9120d8307")).size());

	set<string> reference_set;
	for(unsigned j = 1; j < 11; ++j)
	{
		reference_set.insert(string("totam.sunt.consequatur.numquam.aperiam") + to_string(j));
	}
	for(const auto& item : ret)
	{
		set<string> found_set;
		for(const auto& m : std::get<0>(item.second))
		{
			found_set.insert(m.name());
		}
		for(const auto& ref : reference_set)
		{
			EXPECT_TRUE(found_set.find(ref) != found_set.end()) << ref << " not found for " << item.first;
		}
	}
	fclose(output_file);
	fclose(input_fd);
}

// same as the parser test, but we have a line in there longer than the buffer size to ensure it gets nuked from space properly
TEST(statsite_proxy, parser_long)
{
	auto output_file = fopen("resources/statsite_output_long.txt", "r");
	auto input_fd = fopen("/dev/null", "w");
	ASSERT_TRUE(output_file != NULL);

	// 300 chosen to be smaller than the longest string in the above file
	statsite_proxy proxy(make_pair(input_fd, output_file), false);

	auto ret = proxy.read_metrics();
	ASSERT_EQ(1U, ret.size());
	EXPECT_EQ(1U, std::get<0>(ret.at("")).size());

	set<string> reference_set;

	// must match the string in the above file. Chosen to be longer than the
	// default max length above which we have to reallocate a larger buffer and
	// log a comment
	reference_set.insert(string("totam.sunt.consequatur.numquam.aperiamRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRREEEEEEEEEEEEEEEEEEEEEEEEEEEEEALLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYLLLLLLLLLLLLLLLLLLLLLLOOOOOOOOOOOOOOOOOOOOOOOONNNNNNNNNNNNNNNNNNNNGGGGGGGGGGGGG"));
	for(const auto& item : ret)
	{
		set<string> found_set;
		for(const auto& m : std::get<0>(item.second))
		{
			found_set.insert(m.name());
		}
		for(const auto& ref : reference_set)
		{
			EXPECT_TRUE(found_set.find(ref) != found_set.end()) << ref << " not found for " << item.first;
		}
	}
	fclose(output_file);
	fclose(input_fd);
}

namespace
{

/**
 * Send the given stats to statsite_proxy and let it validate it for
 * correctness.  If it's valid, statsite_proxy will write it to the appropriate
 * FILE*, which we can later inspect.
 *
 * @param[in] stats    a potential statsd metric (valid or invalid).
 * @param[in] expected What the client expects statsite_proxy to write to 
 *                     the FILE*.
 */
void do_statsite_proxy_validation(const std::string& stats,
		                  const std::string& expected)
{
	char in_buffer[512] = {};
	char out_buffer[2] = {};
	FILE* const in = fmemopen(in_buffer, sizeof(in_buffer) - 1, "r+");
	FILE* const out = fmemopen(out_buffer, sizeof(out_buffer) - 1, "r+");

	ASSERT_TRUE(in != nullptr);
	ASSERT_TRUE(out != nullptr);

	ASSERT_NO_THROW({
		statsite_proxy proxy(make_pair(in, out), true);

		proxy.send_metric(stats.c_str(), stats.size());
	});

	ASSERT_EQ(0, fclose(in));
	ASSERT_EQ(0, fclose(out));

	ASSERT_EQ(expected, std::string(in_buffer));
}

}

/**
 * Ensure that writing a single metric without a trailing newline is validated
 */
TEST(statsite_proxy, single_metric_no_newline_validated)
{
	const std::string valid_metric = "a:b|c";
	const std::string expected = valid_metric + "\n";

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing a single metric with a trailing newline is validated.
 */
TEST(statsite_proxy, single_metric_no_newline_extra_pipe_validated)
{
	const std::string valid_metric = "a:b|c\n";
	const std::string expected = valid_metric;

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing a single metric with an extra pipe and without a
 * trailing newline is validated.
 */
TEST(statsite_proxy, single_metric_extra_pipe_no_newline)
{
	const std::string valid_metric = "a:b|c|d";
	const std::string expected = valid_metric + "\n";

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing a single metric with an extra pipe and with a
 * trailing newline is validated.
 */
TEST(statsite_proxy, single_metric_extra_pipe_newline)
{
	const std::string valid_metric = "a:b|c|d\n";
	const std::string expected = valid_metric;

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing multiple metrics without a trailing newline is validated
 */
TEST(statsite_proxy, multiple_metrics_no_newline_validated)
{
	const std::string valid_metric = "a:b|c\nd:e|f";
	const std::string expected = valid_metric + "\n";

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing multiple metrics with a trailing newline is validated
 */
TEST(statsite_proxy, multiple_metrics_newline_validated)
{
	const std::string valid_metric = "a:b|c\nd:e|f\n";
	const std::string expected = valid_metric;

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing multiple metrics with extra pipes, without a trailing
 * newline is validated
 */
TEST(statsite_proxy, multiple_metrics_extra_pipe_no_newline_validated)
{
	const std::string valid_metric = "a:b|c|d\ne:f|g|h";
	const std::string expected = valid_metric + "\n";

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing multiple metrics with extra pipes, with a trailing
 * newline is validated
 */
TEST(statsite_proxy, multiple_metrics_extra_pipe_newline_validated)
{
	const std::string valid_metric = "a:b|c|d\ne:f|g|h\n";
	const std::string expected = valid_metric;

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing a single metric with multiple characters and no newline
 * is validated.
 */
TEST(statsite_proxy, single_metric_multiple_characters_no_newline)
{
	const std::string valid_metric = "abc:defg|hijklmn";
	const std::string expected = valid_metric + "\n";

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing a single metric with multiple characters and a trailing
 * newline is validated.
 */
TEST(statsite_proxy, single_metric_multiple_characters_newline)
{
	const std::string valid_metric = "abc:defg|hijklmn\n";
	const std::string expected = valid_metric;

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing multiple metrics with multiple characters and no
 * trailing newline is validated.
 */
TEST(statsite_proxy, multiple_metrics_multiple_characters_no_newline)
{
	const std::string valid_metric = "abc:defg|hijklmn\nop:qr|st";
	const std::string expected = valid_metric + "\n";

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing multiple metrics with multiple characters and a
 * trailing newline is validated.
 */
TEST(statsite_proxy, multiple_metrics_multiple_characters_newline)
{
	const std::string valid_metric = "abc:defg|hijklmn\nop:qr|st\n";
	const std::string expected = valid_metric;

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing multiple metrics with multiple characters, multiple
 * pipes, and no trailing newline is validated.
 */
TEST(statsite_proxy, multiple_metrics_multiple_characters_extra_pipe_no_newline)
{
	const std::string valid_metric = "abc:defg|hijklmn|zzz\nop:qr|st|xxx";
	const std::string expected = valid_metric + "\n";

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that writing multiple metrics with multiple characters, multiple
 * pipes, and with a trailing newline is validated.
 */
TEST(statsite_proxy, multiple_metrics_multiple_characters_extra_pipe_newline)
{
	const std::string valid_metric = "abc:defg|hijklmn|zzz\nop:qr|st|xxx\n";
	const std::string expected = valid_metric;

	do_statsite_proxy_validation(valid_metric, expected);
}

/**
 * Ensure that if a colon is missing, the metric is not validated.
 */
TEST(statsite_proxy, single_metric_missing_colon_invalid)
{
	const std::string invalid_metric = "ab|c";
	const std::string expected = "";

	do_statsite_proxy_validation(invalid_metric, expected);
}

/**
 * Ensure that if a colon is missing, the entire buffer is not validated.
 */
TEST(statsite_proxy, multiple_metrics_missing_colon_invalid)
{
	const std::string invalid_metric = "ab|c\na:b|c";
	const std::string expected = "";

	do_statsite_proxy_validation(invalid_metric, expected);
}

/**
 * Ensure that if a pipe is missing, the metric is not validated.
 */
TEST(statsite_proxy, single_metric_missing_pipe_invalid)
{
	const std::string invalid_metric = "a:bc";
	const std::string expected = "";

	do_statsite_proxy_validation(invalid_metric, expected);
}

/**
 * Ensure that if a pipe is missing, the entire buffer is not validated.
 */
TEST(statsite_proxy, multiple_metrics_missing_pipe_invalid)
{
	const std::string invalid_metric = "a:bc\na:b|c";
	const std::string expected = "";

	do_statsite_proxy_validation(invalid_metric, expected);
}

/**
 * Ensure that a metric containing only pipes is invalid.
 */
TEST(statsite_proxy, only_pipes_invalid)
{
	const std::string invalid_metric = "a|b|c";
	const std::string expected = "";

	do_statsite_proxy_validation(invalid_metric, expected);
}

/**
 * Ensure that a metric containing only colons is invalid.
 */
TEST(statsite_proxy, only_colons_invalid)
{
	const std::string invalid_metric = "a:b:c";
	const std::string expected = "";

	do_statsite_proxy_validation(invalid_metric, expected);
}

/**
 * Ensure that having extra colons in various invalid places is not
 * validated.
 */
TEST(statsite_proxy, random_extra_colons_invalid)
{
	const std::string expected = "";

	do_statsite_proxy_validation(":a:b|c\na:b|c", expected);
	do_statsite_proxy_validation("a::b|c\na:b|c", expected);
	do_statsite_proxy_validation("a:b|:c\na:b|c", expected);
	do_statsite_proxy_validation("a:b|c\n:a:b|c", expected);
	do_statsite_proxy_validation("a:b|c\na:b:|c", expected);
	do_statsite_proxy_validation("a:b|c\na:b:|c:", expected);
}

/**
 * Ensure that having extra pipes in various invalid places is not
 * validated.
 */
TEST(statsite_proxy, random_extra_pipes_invalid)
{
	const std::string expected = "";

	do_statsite_proxy_validation("|a:b|c\na:b|c", expected);
	do_statsite_proxy_validation("a|:b|c\na:b|c", expected);
	do_statsite_proxy_validation("a:b||c\na:b|c", expected);
	do_statsite_proxy_validation("a:b|c\n|a:b|c", expected);
	do_statsite_proxy_validation("a:b|c\na:b||c", expected);
	do_statsite_proxy_validation("a:b|c\na:b:|c|", expected);
}

/**
 * Ensure that having extra newlines in various invalid places is not
 * validated.
 */
TEST(statsite_proxy, random_extra_newlines_invalid)
{
	const std::string expected = "";

	do_statsite_proxy_validation("\na:b|c\na:b|c", expected);
	do_statsite_proxy_validation("a:\nb|c\na:b|c", expected);
	do_statsite_proxy_validation("a:b|\nc\na:b|c", expected);
	do_statsite_proxy_validation("a:b|c\n\na:b|c", expected);
	do_statsite_proxy_validation("a:b|c\na:b\n|c", expected);
}

TEST(statsite_proxy, filter)
{
	auto output_file = fopen("resources/statsite_output.txt", "r");
	auto input_fd = fopen("/dev/null", "w");
	ASSERT_TRUE(output_file != NULL);
	statsite_proxy proxy(make_pair(input_fd, output_file), false);

	filter_vec_t f({{"totam.sunt.consequatur.numquam.aperiam5", true}, {"totam.*", false}});
	metric_limits::sptr_t ml(new metric_limits(f));
	auto ret = proxy.read_metrics(ml);
	EXPECT_EQ(2U, ret.size());
	EXPECT_EQ(1U, std::get<0>(ret.at("")).size());
	EXPECT_EQ(1U, std::get<0>(ret.at("3ce9120d8307")).size());

	fclose(output_file);
	fclose(input_fd);

	output_file = fopen("resources/statsite_output.txt", "r");
	input_fd = fopen("/dev/null", "w");
	ASSERT_TRUE(output_file != NULL);
	statsite_proxy proxy2(make_pair(input_fd, output_file), false);

	f = {{"*1?", true}, {"totam.sunt.consequatur.numquam.aperiam7", true}, {"*", false}};
	ml.reset(new metric_limits(f));
	ret = proxy2.read_metrics(ml);
	EXPECT_EQ(2U, ret.size());
	EXPECT_EQ(2U, std::get<0>(ret.at("")).size());
	EXPECT_EQ(2U, std::get<0>(ret.at("3ce9120d8307")).size());

	fclose(output_file);
	fclose(input_fd);
}
