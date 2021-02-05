#define VISIBILITY_PRIVATE

#include "event_capture.h"
#include "uncompressed_sample_handler.h"

#include "userspace-shared/test-helpers/scoped_config.h"

#include <gtest.h>
#include <sinsp.h>
#include <unistd.h>

#define ONE_SECOND_MS 1000

namespace
{
uncompressed_sample_handler_dummy g_sample_handler;
audit_tap_handler_dummy g_audit_handler;
null_secure_audit_handler g_secure_audit_handler;
null_secure_profiling_handler g_secure_profiling_handler;
null_secure_netsec_handler g_secure_netsec_handler;
}  // namespace

using namespace test_helpers;

void event_capture::capture()
{
	scoped_config<bool> config("autodrop.enabled", false);

	m_inspector = new sinsp();
	internal_metrics::sptr_t int_metrics = std::make_shared<internal_metrics>();
	m_analyzer = new sinsp_analyzer(m_inspector,
	                                "/opt/draios",
	                                int_metrics,
	                                g_audit_handler,
	                                g_secure_audit_handler,
	                                g_secure_profiling_handler,
	                                g_secure_netsec_handler,
	                                &m_flush_queue,
	                                []() -> bool { return true; });
	m_inspector->register_external_event_processor(*m_analyzer);

	m_analyzer->set_configuration(m_configuration);

	if (m_max_thread_table_size != 0)
	{
		m_inspector->m_thread_manager->set_max_thread_table_size(m_max_thread_table_size);
	}

	if (m_thread_timeout_ns != 0)
	{
		m_inspector->m_thread_timeout_ns = m_thread_timeout_ns;
	}

	if (m_inactive_thread_scan_time_ns != 0)
	{
		m_inspector->m_inactive_thread_scan_time_ns = m_inactive_thread_scan_time_ns;
	}

	m_inspector->set_get_procs_cpu_from_driver(true);

	m_param.m_inspector = m_inspector;
	m_param.m_analyzer = m_analyzer;

	m_before_open(m_inspector, m_analyzer);

	ASSERT_FALSE(m_inspector->is_capture());
	ASSERT_FALSE(m_inspector->is_live());
	ASSERT_FALSE(m_inspector->is_nodriver());

	try
	{
		if (m_mode == SCAP_MODE_NODRIVER)
		{
			m_inspector->open_nodriver();
		}
		else
		{
			m_inspector->open(ONE_SECOND_MS);
		}
	}
	catch (sinsp_exception& e)
	{
		m_start_failed = true;
		m_start_failure_message =
		    "couldn't open inspector (maybe driver hasn't been loaded yet?) err=" +
		    m_inspector->getlasterr() + " exception=" + e.what();
		m_capture_started.set();
		delete m_inspector;
		delete m_analyzer;
		return;
	}

	const ::testing::TestInfo* const test_info =
	    ::testing::UnitTest::GetInstance()->current_test_info();

	m_inspector->set_debug_mode(true);
	m_inspector->set_hostname_and_port_resolution_mode(false);

	if (m_mode != SCAP_MODE_NODRIVER)
	{
		m_dump_filename = std::string("./captures/") + test_info->test_case_name() + "_" +
		                  test_info->name() + ".scap";
		try
		{
			m_inspector->autodump_start(m_dump_filename, false);
		}
		catch (std::exception& e)
		{
			m_start_failed = true;
			m_start_failure_message =
			    std::string("couldn't start dumping to ") + m_dump_filename + ": " + e.what();
			m_capture_started.set();
			delete m_inspector;
			delete m_analyzer;
			return;
		}
	}

	bool signaled_start = false;
	sinsp_evt* event;
	bool result = true;
	int32_t next_result = SCAP_SUCCESS;
	while (!m_stopped && result && !::testing::Test::HasFatalFailure())
	{
		if (SCAP_SUCCESS == (next_result = m_inspector->next(&event)))
		{
			result = handle_event(event);
		}
		if (!signaled_start)
		{
			signaled_start = true;
			m_capture_started.set();
		}
	}

	if (m_mode != SCAP_MODE_NODRIVER)
	{
		m_inspector->stop_capture();

		uint32_t n_timeouts = 0;
		while (result && !::testing::Test::HasFatalFailure())
		{
			next_result = m_inspector->next(&event);
			if (next_result == SCAP_TIMEOUT)
			{
				n_timeouts++;

				if (n_timeouts < m_max_timeouts)
				{
					continue;
				}
				else
				{
					break;
				}
			}

			if (next_result != SCAP_SUCCESS)
			{
				break;
			}
			result = handle_event(event);
		}
		while (SCAP_SUCCESS == m_inspector->next(&event))
		{
			// just consume the events
		}
	}

	m_before_close(m_inspector, m_analyzer);

	delete m_inspector;
	delete m_analyzer;
	m_capture_stopped.set();

	if (scap_get_bpf_probe_from_env() == NULL)
	{
		// At this point there should be no consumers of the driver remaining.
		// Wait up to 2 seconds for the refcount to settle
		int ntries = 20;
		uint32_t num_consumers = 0;

		while (ntries-- > 0)
		{
			FILE* ref = fopen("/sys/module/sysdigcloud_probe/refcnt", "r");
			ASSERT_TRUE(ref != NULL);

			ASSERT_EQ(fscanf(ref, "%u", &num_consumers), 1);
			fclose(ref);

			if (num_consumers == 0)
			{
				break;
			}
			struct timespec ts = {
			    .tv_sec = 0,
			    .tv_nsec = 100000000,
			};
			nanosleep(&ts, NULL);
		}
		ASSERT_EQ(num_consumers, 0u);
	}
}
