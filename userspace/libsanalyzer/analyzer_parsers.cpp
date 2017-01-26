#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

#include "sinsp.h"
#include "sinsp_int.h"
#include "../../driver/ppm_ringbuffer.h"

#ifdef HAS_ANALYZER
#include "parsers.h"
#include "analyzer_int.h"
#include "analyzer.h"
#include "analyzer_parsers.h"
#include "metrics.h"
#undef min
#undef max
#include "draios.pb.h"
#include "delays.h"
#include "scores.h"
#include "procfs_parser.h"
#include "sinsp_errno.h"
#include "sched_analyzer.h"
#include "analyzer_thread.h"
#include "analyzer_fd.h"

sinsp_analyzer_parsers::sinsp_analyzer_parsers(sinsp_analyzer* analyzer)
{
	m_analyzer = analyzer;
	m_sched_analyzer2 = NULL;
	m_last_drop_was_enter = false;
}

void sinsp_analyzer_parsers::on_capture_start()
{
	m_sched_analyzer2 = m_analyzer->m_sched_analyzer2;
}

//
// This is similar to sinsp_parser::process_event, but it's for draios-only event 
// processing. Returns false if process_event() should return immediately.
//
bool sinsp_analyzer_parsers::process_event(sinsp_evt* evt)
{
	uint16_t etype = evt->get_type();

	switch(etype)
	{
	case PPME_SCHEDSWITCH_1_E:
	case PPME_SCHEDSWITCH_6_E:
		if(m_analyzer->m_inspector->m_thread_manager->get_thread_count() < DROP_SCHED_ANALYZER_THRESHOLD)
		{
			m_sched_analyzer2->process_event(evt);
		}
		return false;
	case PPME_SOCKET_ACCEPT_X:
	case PPME_SOCKET_ACCEPT4_X:
	case PPME_SOCKET_ACCEPT_5_X:
	case PPME_SOCKET_ACCEPT4_5_X:
		parse_accept_exit(evt);;
		return true;
	case PPME_SYSCALL_SELECT_X:
	case PPME_SYSCALL_POLL_X:
	case PPME_SYSCALL_EPOLLWAIT_X:
		parse_select_poll_epollwait_exit(evt);
		return true;
	case PPME_SYSCALL_EXECVE_8_X:
	case PPME_SYSCALL_EXECVE_13_X:
	case PPME_SYSCALL_EXECVE_14_X:
	case PPME_SYSCALL_EXECVE_15_X:
	case PPME_SYSCALL_EXECVE_16_X:
		parse_execve_exit(evt);
		return true;
	case PPME_DROP_E:
		if(!m_last_drop_was_enter)
		{
			parse_drop(evt);

#ifdef SIMULATE_DROP_MODE
			//
			// Set dropping mode
			//
			m_analyzer->m_inspector->m_isdropping = true;
#endif
			//g_logger.log("Executing flush (drop_e)", sinsp_logger::SEV_INFO);
			m_analyzer->flush(evt, evt->get_ts(), false, sinsp_analyzer::DF_FORCE_FLUSH);

			m_last_drop_was_enter = true;
		}

		return false;
	case PPME_DROP_X:
		if(m_last_drop_was_enter)
		{
			parse_drop(evt);

#ifdef SIMULATE_DROP_MODE
			//
			// Turn off dropping mode
			//
			m_analyzer->m_inspector->m_isdropping = false;
#endif
			//g_logger.log("Executing flush (drop_x)", sinsp_logger::SEV_INFO);
			m_analyzer->flush(evt, evt->get_ts(), false, sinsp_analyzer::DF_FORCE_FLUSH_BUT_DONT_EMIT);

			m_last_drop_was_enter = false;
		}

		return false;

	case PPME_SYSDIGEVENT_E:
		m_analyzer->m_driver_stopped_dropping = true;
		return false;
	case PPME_CONTAINER_E:
		return false;
	default:
		return true;
	}
}

void sinsp_analyzer_parsers::parse_accept_exit(sinsp_evt* evt)
{
	//
	// Extract the request queue length 
	//
	sinsp_evt_param *parinfo = evt->get_param(2);
	ASSERT(parinfo->m_len == sizeof(uint8_t));
	uint8_t queueratio = *(uint8_t*)parinfo->m_val;
	ASSERT(queueratio <= 100);

	if(evt->m_tinfo == NULL)
	{
		return;
	}

	if(queueratio > evt->m_tinfo->m_ainfo->m_connection_queue_usage_pct)
	{
		evt->m_tinfo->m_ainfo->m_connection_queue_usage_pct = queueratio;
	}

	//
	// If this comes after a wait, reset the last wait time, since we don't count 
	// time waiting for an accept as I/O time
	//
	evt->m_tinfo->m_ainfo->m_last_wait_duration_ns = 0;
}

void sinsp_analyzer_parsers::parse_select_poll_epollwait_exit(sinsp_evt *evt)
{
	sinsp_evt_param *parinfo;
	int64_t retval;
	uint16_t etype = evt->get_type();

	if(evt->m_tinfo == NULL)
	{
		return;
	}

	if(etype != evt->m_tinfo->m_lastevent_type + 1)
	{
		//
		// Packet drop. Previuos event didn't have a chance to 
		//
		return;
	}

	//
	// Extract the return value
	//
	parinfo = evt->get_param(0);
	retval = *(int64_t *)parinfo->m_val;
	ASSERT(parinfo->m_len == sizeof(int64_t));

	//
	// Check if the syscall was successful
	//
	if(retval >= 0)
	{
		sinsp_threadinfo* tinfo = evt->m_tinfo;

		if(tinfo == NULL)
		{
			ASSERT(false);
			return;
		}

		if(tinfo->is_lastevent_data_valid() && evt->m_tinfo->m_lastevent_data)
		{
			//
			// We categorize this based on the next I/O operation only if the number of 
			// FDs that were waited for is 1
			//
			if(retval == 0)
			{
				tinfo->m_ainfo->m_last_wait_duration_ns = 0;
			}
			else
			{
				//
				// If this was a wait on a *single* fd, we can easily categorize it with certainty and
				// we encode the delta as a positive number.
				// If this was a wait on multiple FDs, we encode the delta as a negative number so
				// the next steps will know that it needs to be handled with more care.
				//
				uint64_t sample_duration = m_analyzer->m_configuration->get_analyzer_sample_len_ns();
				uint64_t ts = evt->get_ts();

				tinfo->m_ainfo->m_last_wait_end_time_ns = ts;
				uint64_t start_time_ns = MAX(ts - ts % sample_duration, *(uint64_t*)evt->m_tinfo->m_lastevent_data);

				if(retval == 1)
				{
					tinfo->m_ainfo->m_last_wait_duration_ns = ts - start_time_ns;
				}
				else
				{
					tinfo->m_ainfo->m_last_wait_duration_ns = start_time_ns - ts;
				}
			}
		}
	}
}

//
// This code tries to optimize the bacward navigation by caching the login
// shell ID and the indentation factor. It's incomplete and currently 
// commented as we determine if it's really useful.
//
#if 0
void sinsp_analyzer_parsers::patch_ancestor_chain(vector<thread_analyzer_info*>* ancestor_chain, bool is_interactive, uint64_t session_id)
{
	for(vector<thread_analyzer_info*>::iterator a = ancestor_chain->begin(); a != ancestor_chain->end(); ++a)
	{
		if(is_interactive)
		{
			(*a)->m_th_analysis_flags |= thread_analyzer_info::AF_IS_DESCENDENT_OF_SHELL;
			(*a)->m_th_analysis_flags &= ~thread_analyzer_info::AF_IS_NOT_DESCENDENT_OF_SHELL;
		}
		else
		{
			(*a)->m_th_analysis_flags |= thread_analyzer_info::AF_IS_NOT_DESCENDENT_OF_SHELL;
			(*a)->m_th_analysis_flags &= ~thread_analyzer_info::AF_IS_DESCENDENT_OF_SHELL;
		}

		(*a)->m_login_shell_id = session_id;
	}
}
#endif

void sinsp_analyzer_parsers::parse_execve_exit(sinsp_evt* evt)
{
	sinsp_threadinfo* tinfo = evt->get_thread_info();
	if(tinfo == NULL)
	{
		return;
	}

	//
	// Check the result of the call
	//
	sinsp_evt_param *parinfo;
	parinfo = evt->get_param(0);
	ASSERT(parinfo->m_len == sizeof(int64_t));
	int64_t res = *(int64_t*)parinfo->m_val;

	if(res < 0)
	{
		return;
	}

	thread_analyzer_info* tainfo = evt->m_tinfo->m_ainfo;
	tainfo->m_called_execve = true;

	//
	// Allocated an executed command storage info and initialize it
	//
	sinsp_executed_command cmdinfo;

	if(tinfo->m_clone_ts != 0)
	{
		cmdinfo.m_ts = tinfo->m_clone_ts;
	}
	else
	{
		cmdinfo.m_ts = evt->get_ts();
	}

	cmdinfo.m_cmdline = tinfo->m_comm;
	cmdinfo.m_exe = tinfo->m_exe;
	cmdinfo.m_comm = tinfo->m_comm;
	cmdinfo.m_pid = tinfo->m_pid;
	cmdinfo.m_ppid = tinfo->m_ptid;
	cmdinfo.m_uid = tinfo->m_uid;
	cmdinfo.m_cwd = tinfo->m_cwd;

	//
	// Build the arguments string
	//
	uint32_t nargs = tinfo->m_args.size();

	for(uint32_t j = 0; j < nargs; j++)
	{
		cmdinfo.m_cmdline += ' ';
		cmdinfo.m_cmdline += tinfo->m_args[j];
	}

	//
	// Navigate the parent processes to determine if this is the descendent of a shell
	// and if yes what's the shell ID
	//
	vector<thread_analyzer_info*> ancestor_chain;

	sinsp_threadinfo* ttinfo = tinfo;
	//bool shell_found = false;
	uint32_t shell_dist = 0;
	uint64_t login_shell_id = 0;

	for(uint32_t j = 0; ; j++)
	{
		thread_analyzer_info* ttainfo = ttinfo->m_ainfo;
		ancestor_chain.push_back(ttainfo);

		//
		// This code tries to optimize the bacward navigation by caching the login
		// shell ID and the indentation factor. It's incomplete and currently 
		// commented as we determine if it's really useful.
		//
#if 0
		if(ttainfo->m_th_analysis_flags & thread_analyzer_info::AF_IS_DESCENDENT_OF_SHELL)
		{
			//
			// This thread has an ancestor which is the descendent of a shell.
			//
			patch_ancestor_chain(&ancestor_chain, true, ttainfo->m_login_shell_id);
			break;
		}
		else if(ttainfo->m_th_analysis_flags & thread_analyzer_info::AF_IS_NOT_DESCENDENT_OF_SHELL)
		{
			//
			// This thread has an ancestor which is NOT the descendent of a shell.
			// This means that we don't add it and we stop here. 
			//
			patch_ancestor_chain(&ancestor_chain, false, 0);
			return;
		}
#endif

		uint32_t cl = ttinfo->m_comm.size();
		if(cl >= 2 && ttinfo->m_comm[cl - 2] == 's' && ttinfo->m_comm[cl - 1] == 'h')
		{
			//
			// We found a shell. Patch the descendat but don't stop here since there might
			// be another parent shell
			//
#if 0			
			patch_ancestor_chain(&ancestor_chain, true, ttinfo->m_tid);
			shell_found = true;
#endif			
			login_shell_id = ttinfo->m_tid;
			shell_dist = j;
		}

		ttinfo = ttinfo->get_parent_thread();
		if(ttinfo == NULL)
		{
#if 0
			if(shell_found == false)
			{
				patch_ancestor_chain(&ancestor_chain, false, 0);
			}
#endif
			break;
		}
	}

	//tainfo->m_login_shell_distance = shell_dist;
	cmdinfo.m_shell_id = login_shell_id;
	cmdinfo.m_login_shell_distance = shell_dist;

	//
	// Determine if this command was executed in a pipe and if yes
	// where it belongs in the pipe
	//
	sinsp_fdinfo_t* fd0 = tinfo->get_fd(0);
	sinsp_fdinfo_t* fd1 = tinfo->get_fd(1);

	if(fd0 && fd1)
	{
		if(fd0->m_type == SCAP_FD_FIFO && fd1->m_type == SCAP_FD_FIFO)
		{
			cmdinfo.m_flags |= sinsp_executed_command::FL_PIPE_MIDDLE;
		}
		else if(fd1->m_type == SCAP_FD_FIFO)
		{
			cmdinfo.m_flags |= sinsp_executed_command::FL_PIPE_HEAD;
		}
		else if(fd0->m_type == SCAP_FD_FIFO)
		{
			cmdinfo.m_flags |= sinsp_executed_command::FL_PIPE_TAIL;
		}
	}

	m_analyzer->m_executed_commands[tinfo->m_container_id].push_back(cmdinfo);

	return;
}

void sinsp_analyzer_parsers::parse_drop(sinsp_evt* evt)
{
	m_analyzer->m_last_dropmode_switch_time = evt->get_ts();

	//
	// If required, update the sample length
	//
	sinsp_evt_param *parinfo;
	parinfo = evt->get_param(0);
	ASSERT(parinfo->m_len == sizeof(int32_t));

	if(*(uint32_t*)parinfo->m_val != m_analyzer->m_sampling_ratio)
	{
		g_logger.format(sinsp_logger::SEV_INFO, "sinsp Switching sampling ratio from % " PRIu32 " to %" PRIu32,
			m_analyzer->m_sampling_ratio,
			*(uint32_t*)parinfo->m_val);

		m_analyzer->set_sampling_ratio(*(int32_t*)parinfo->m_val);
	}
}

#endif // HAS_ANALYZER
