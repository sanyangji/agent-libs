#include <asm/atomic.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kdev_t.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <asm/syscall.h>
#include <net/sock.h>
#if defined(__x86_64__)
#include <asm/unistd_64.h>
#else
#include <asm/unistd_32.h>
#endif

#include "ppm_ringbuffer.h"
#include "ppm_events_public.h"
#include "ppm_events.h"
#include "ppm.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Draios");

#define PPM_DEVICE_NAME "ppm"

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,35))
    #define TRACEPOINT_PROBE_REGISTER(p1, p2) tracepoint_probe_register(p1, p2)
    #define TRACEPOINT_PROBE_UNREGISTER(p1, p2) tracepoint_probe_unregister(p1, p2)
    #define TRACEPOINT_PROBE(probe, args...) static void probe(args)
#else
    #define TRACEPOINT_PROBE_REGISTER(p1, p2) tracepoint_probe_register(p1, p2, 0)
    #define TRACEPOINT_PROBE_UNREGISTER(p1, p2) tracepoint_probe_unregister(p1, p2, 0)
    #define TRACEPOINT_PROBE(probe, args...) static void probe(void* __data, args)
#endif

///////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////
// FORWARD DECLARATIONS
///////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////
static int ppm_open(struct inode *inode, struct file *filp);
static int ppm_release(struct inode *inode, struct file *filp);
static long ppm_ioctl(struct file *f, unsigned int cmd, unsigned long arg);
static int ppm_mmap(struct file *filp, struct vm_area_struct *vma);

TRACEPOINT_PROBE(syscall_enter_probe, struct pt_regs *regs, long id);
TRACEPOINT_PROBE(syscall_exit_probe, struct pt_regs *regs, long ret);
TRACEPOINT_PROBE(syscall_procexit_probe, struct pt_regs *regs, long ret);
TRACEPOINT_PROBE(netif_rx_probe, struct sk_buff *skb);

static struct ppm_device* g_ppm_devs;
static struct class *g_ppm_class = NULL;
static unsigned int g_ppm_numdevs;
static int g_ppm_major;
static struct file_operations g_ppm_fops =
{
	.open = ppm_open,
	.release = ppm_release,
	.mmap = ppm_mmap,
	.unlocked_ioctl = ppm_ioctl,
	.owner = THIS_MODULE,
};

///////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////
// GLOBALS
///////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////

DEFINE_PER_CPU(struct ppm_ring_buffer_context*, g_ring_buffers);
atomic_t g_open_count;

///////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////
// user I/O functions
///////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////
static int ppm_open(struct inode *inode, struct file *filp)
{
	int ret;
	struct ppm_ring_buffer_context* ring;
	int ring_no = iminor(filp->f_dentry->d_inode);

	trace_enter();

	ring = per_cpu(g_ring_buffers, ring_no);

	// XXX this should be atomic
	if(ring->state != CS_STOPPED)
	{
		printk(KERN_INFO "PPM: invalid operation: attempting to open device %d multiple times\n", ring_no);
		return -EBUSY;
	}
	else
	{
		ring->state = CS_STARTED;
	}

	ring->info->head = 0;
	ring->info->tail = 0;
	ring->nevents = 0;
	ring->info->n_evts = 0;
	ring->info->n_drops_buffer = 0;
	ring->info->n_drops_pf = 0;
	ring->info->n_preemptions = 0;
	atomic_set(&ring->preempt_count, 0);
	getnstimeofday(&ring->last_print_time);

	//
	// The last open device starts the collection
	//
	if(atomic_inc_return(&g_open_count) == g_ppm_numdevs)
	{
//		struct task_struct *task;

		printk(KERN_INFO "PPM: starting capture\n");

		/*
				//
				// Before enabling the tracepoints, we add the current list of running processes as events in buffer 0
				//
				for_each_process(task)
				{
					printk("%s [%d]\n",task->comm , task->pid);
				}
		*/

		//
		// Enable the tracepoints
		//
		ret = TRACEPOINT_PROBE_REGISTER("sys_exit", (void *) syscall_exit_probe);
		if(ret)
		{
			printk(KERN_ERR "PPM: can't create the sys_exit tracepoint\n");
			return ret;
		}

		ret = TRACEPOINT_PROBE_REGISTER("sys_enter", (void *) syscall_enter_probe);
		if(ret)
		{
			TRACEPOINT_PROBE_UNREGISTER("sys_exit",
			                            (void *) syscall_exit_probe);

			printk(KERN_ERR "PPM: can't create the sys_enter tracepoint\n");

			return ret;
		}

		ret = TRACEPOINT_PROBE_REGISTER("sched_process_exit", (void *) syscall_procexit_probe);
		if(ret)
		{
			TRACEPOINT_PROBE_UNREGISTER("sys_exit",
			                            (void *) syscall_exit_probe);
			TRACEPOINT_PROBE_UNREGISTER("sys_enter",
			                            (void *) syscall_enter_probe);

			printk(KERN_ERR "PPM: can't create the sched_process_exit tracepoint\n");

			return ret;
		}

		ret = TRACEPOINT_PROBE_REGISTER("netif_rx", (void *) netif_rx_probe);
		if(ret)
		{
			TRACEPOINT_PROBE_UNREGISTER("sys_exit",
			                            (void *) syscall_exit_probe);
			TRACEPOINT_PROBE_UNREGISTER("sys_enter",
			                            (void *) syscall_enter_probe);
			TRACEPOINT_PROBE_UNREGISTER("sched_process_exit",
			                            (void *) syscall_procexit_probe);

			printk(KERN_ERR "PPM: can't create the netif_rx tracepoint\n");

			return ret;
		}
	}

	return 0;
}

static int ppm_release(struct inode *inode, struct file *filp)
{
	struct ppm_ring_buffer_context* ring;
	int ring_no = iminor(filp->f_dentry->d_inode);

	trace_enter();

	ring = per_cpu(g_ring_buffers, ring_no);

	// XXX this should be atomic
	if(ring->state == CS_STOPPED)
	{
		printk(KERN_INFO "PPM: attempting to close unopened device %d\n", ring_no);
		return -EBUSY;
	}
	else
	{
		ring->state = CS_STOPPED;
	}

	printk(KERN_INFO "PPM: closing ring %d, evt:%llu, dr_buf:%llu, dr_pf:%llu, pr:%llu\n",
	       ring_no,
	       ring->info->n_evts,
	       ring->info->n_drops_buffer,
	       ring->info->n_drops_pf,
	       ring->info->n_preemptions);

	//
	// The last closed device stops event collection
	//
	if(atomic_dec_return(&g_open_count) == 0)
	{
		printk(KERN_INFO "PPM: stopping capture\n");

		TRACEPOINT_PROBE_UNREGISTER("sys_exit",
		                            (void *) syscall_exit_probe);

		TRACEPOINT_PROBE_UNREGISTER("sys_enter",
		                            (void *) syscall_enter_probe);

		TRACEPOINT_PROBE_UNREGISTER("sched_process_exit",
		                            (void *) syscall_procexit_probe);

		TRACEPOINT_PROBE_UNREGISTER("netif_rx",
		                            (void *) netif_rx_probe);
	}

	return 0;
}

static long ppm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd)
	{
	case PPM_IOCTL_DISABLE_CAPTURE:
	{
		int ring_no = iminor(filp->f_dentry->d_inode);
		struct ppm_ring_buffer_context* ring = per_cpu(g_ring_buffers, ring_no);

		// xxx should be atomic
		ring->state = CS_INACTIVE;

		printk(KERN_INFO "PPM: PPM_IOCTL_DISABLE_CAPTURE for ring %d\n", ring_no);

		return 0;
	}
	case PPM_IOCTL_ENABLE_CAPTURE:
	{
		int ring_no = iminor(filp->f_dentry->d_inode);
		struct ppm_ring_buffer_context* ring = per_cpu(g_ring_buffers, ring_no);

		// xxx should be atomic
		ring->state = CS_STARTED;

		printk(KERN_INFO "PPM: PPM_IOCTL_ENABLE_CAPTURE for ring %d\n", ring_no);

		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static int ppm_mmap(struct file *filp, struct vm_area_struct *vma)
{
	trace_enter();

	if(vma->vm_pgoff == 0)
	{
		int ret;
		long length = vma->vm_end - vma->vm_start;
		unsigned long useraddr = vma->vm_start;
		unsigned long pfn;
		char* vmalloc_area_ptr;
		char* orig_vmalloc_area_ptr;
		int ring_no = iminor(filp->f_dentry->d_inode);
		struct ppm_ring_buffer_context* ring;

		printk(KERN_INFO "PPM: mmap for CPU %d, start=%lu len=%ld page_size=%lu\n",
		       ring_no,
		       useraddr,
		       length,
		       PAGE_SIZE);

		//
		// Enforce ring buffer size
		//
		if(RING_BUF_SIZE < 2 * PAGE_SIZE)
		{
			printk(KERN_INFO "PPM: Ring buffer size too small (%ld bytes, must be at least %ld bytes\n",
			       (long)RING_BUF_SIZE,
			       (long)PAGE_SIZE);
			return -EIO;
		}

		if(RING_BUF_SIZE / PAGE_SIZE * PAGE_SIZE != RING_BUF_SIZE)
		{
			printk(KERN_INFO "PPM: Ring buffer size is not a multiple of the page size\n");
			return -EIO;
		}

		//
		// Retrieve the ring structure for this CPU
		//
		ring = per_cpu(g_ring_buffers, ring_no);

		if(length <= PAGE_SIZE)
		{
			//
			// When the size requested by the user is smaller than a page, we assume
			// she's mapping the ring info structure
			//
			printk(KERN_INFO "PPM: mapping the ring info\n");

			vmalloc_area_ptr = (char*)ring->info;
			orig_vmalloc_area_ptr = vmalloc_area_ptr;

			pfn = vmalloc_to_pfn(vmalloc_area_ptr);

			if((ret = remap_pfn_range(vma,
			                          useraddr,
			                          pfn,
			                          PAGE_SIZE,
			                          PAGE_SHARED)) < 0)
			{
				printk(KERN_INFO "PPM: remap_pfn_range failed (1)\n");
				return ret;
			}

			return 0;
		}
		else if(length == RING_BUF_SIZE * 2)
		{
			long mlength;

			//
			// When the size requested by the user equals the ring buffer size, we map the full
			// buffer
			//
			printk(KERN_INFO "PPM: mapping the data buffer\n");

			vmalloc_area_ptr = (char*)ring->buffer;
			orig_vmalloc_area_ptr = vmalloc_area_ptr;

			//
			// Validate that the buffer access is read only
			//
			if(vma->vm_flags & (VM_WRITE | VM_EXEC))
			{
				printk(KERN_INFO "PPM: invalid mmap flags 0x%lx\n", vma->vm_flags);
				return -EIO;
			}

			//
			// Map each single page of the buffer
			//
			mlength = length / 2;

			while(mlength > 0)
			{
				pfn = vmalloc_to_pfn(vmalloc_area_ptr);

				if((ret = remap_pfn_range(vma, useraddr, pfn, PAGE_SIZE, PAGE_SHARED)) < 0)
				{
					printk(KERN_INFO "PPM: remap_pfn_range failed (1)\n");
					return ret;
				}

				useraddr += PAGE_SIZE;
				vmalloc_area_ptr += PAGE_SIZE;
				mlength -= PAGE_SIZE;
			}

			//
			// Remap a second copy of the buffer pages at the end of the buffer.
			// This effectively mirrors the buffer at its end and helps simplify buffer management in userland.
			//
			vmalloc_area_ptr = orig_vmalloc_area_ptr;
			mlength = length / 2;

			while(mlength > 0)
			{
				pfn = vmalloc_to_pfn(vmalloc_area_ptr);

				if((ret = remap_pfn_range(vma, useraddr, pfn, PAGE_SIZE, PAGE_SHARED)) < 0)
				{
					printk(KERN_INFO "PPM: remap_pfn_range failed (1)\n");
					return ret;
				}

				useraddr += PAGE_SIZE;
				vmalloc_area_ptr += PAGE_SIZE;
				mlength -= PAGE_SIZE;
			}

			return 0;
		}
		else
		{
			printk(KERN_INFO "PPM: Invalid mmap size %ld\n", length);
			return -EIO;
		}
	}

	printk(KERN_INFO "PPM: invalid pgoff %lu, must be 0\n", vma->vm_pgoff);
	return -EIO;
}

/* Argument list sizes for sys_socketcall */
#define AL(x) ((x) * sizeof(unsigned long))
static const unsigned char nas[21] =
{
	AL(0), AL(3), AL(3), AL(3), AL(2), AL(3),
	AL(3), AL(3), AL(4), AL(4), AL(4), AL(6),
	AL(6), AL(2), AL(5), AL(5), AL(3), AL(3),
	AL(4), AL(5), AL(4)
};
#undef AL

#ifndef __x86_64__
static enum ppm_event_type parse_socketcall(struct event_filler_arguments* filler_args, struct pt_regs *regs)
{
	unsigned long __user args[2];
	unsigned long __user* scargs;
	int socketcall_id;

	syscall_get_arguments(current, regs, 0, 2, args);
	socketcall_id = args[0];
	scargs = (unsigned long __user*)args[1];

	if(unlikely(socketcall_id < SYS_SOCKET || 
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,0)
		socketcall_id > SYS_SENDMMSG))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33)
		socketcall_id > SYS_RECVMMSG))
#else
		socketcall_id > SYS_ACCEPT4))
#endif
	{
		return PPME_GENERIC_E;
	}
	
	if(unlikely(ppm_copy_from_user(filler_args->socketcall_args, scargs, nas[socketcall_id])))
	{
		return PPME_GENERIC_E;
	}	

	switch(socketcall_id)
	{
	case SYS_SOCKET:
		return PPME_SOCKET_SOCKET_E;
	case SYS_BIND:
		return PPME_SOCKET_BIND_E;
	case SYS_CONNECT:
		return PPME_SOCKET_CONNECT_E;
	case SYS_LISTEN:
		return PPME_SOCKET_LISTEN_E;
	case SYS_ACCEPT:
		return PPME_SOCKET_ACCEPT_E;
	case SYS_GETSOCKNAME:
		return PPME_SOCKET_GETSOCKNAME_E;
	case SYS_GETPEERNAME:
		return PPME_SOCKET_GETPEERNAME_E;
	case SYS_SOCKETPAIR:
		return PPME_SOCKET_SOCKETPAIR_E;
	case SYS_SEND:
		return PPME_SOCKET_SEND_E;
	case SYS_SENDTO:
		return PPME_SOCKET_SENDTO_E;
	case SYS_RECV:
		return PPME_SOCKET_RECV_E;
	case SYS_RECVFROM:
		return PPME_SOCKET_RECVFROM_E;
	case SYS_SHUTDOWN:
		return PPME_SOCKET_SHUTDOWN_E;
	case SYS_SETSOCKOPT:
		return PPME_SOCKET_SETSOCKOPT_E;
	case SYS_GETSOCKOPT:
		return PPME_SOCKET_GETSOCKOPT_E;
	case SYS_SENDMSG:
		return PPME_SOCKET_SENDMSG_E;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,0)
	case SYS_SENDMMSG:
		return PPME_SOCKET_SENDMMSG_E;
#endif
	case SYS_RECVMSG:
		return PPME_SOCKET_RECVMSG_E;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33)
	case SYS_RECVMMSG:
		return PPME_SOCKET_RECVMMSG_E;
#endif
	case SYS_ACCEPT4:
		return PPME_SOCKET_ACCEPT4_E;
	default:
		ASSERT(false);
		return PPME_GENERIC_E;
	}
}
#endif // __x86_64__

static void record_event(enum ppm_event_type event_type, struct pt_regs *regs, long id)
{
	size_t event_size;
	int next;
	uint32_t freespace;
	uint32_t usedspace;
	struct event_filler_arguments args;
	uint32_t ttail;
	uint32_t head;
	struct ppm_ring_buffer_context* ring;
	struct ppm_ring_buffer_info* ring_info;
	int drop = 1;
	int32_t cbres = PPM_SUCCESS;
	struct timespec ts;

	trace_enter();

	getnstimeofday(&ts);

	ring = get_cpu_var(g_ring_buffers);
	ring_info = ring->info;

	//
	// FROM THIS MOMENT ON, WE HAVE TO BE SUPER FAST
	//
	ring_info->n_evts++;

	//
	// Preemption gate
	//
	if(unlikely(atomic_inc_return(&ring->preempt_count) != 1))
	{
		atomic_dec(&ring->preempt_count);
		put_cpu_var(g_ring_buffers);
		ring_info->n_preemptions++;
		ASSERT(false);
		return;
	}

	// xxx access to ring->state should be atomic
	if(unlikely(ring->state == CS_INACTIVE))
	{
		atomic_dec(&ring->preempt_count);
		put_cpu_var(g_ring_buffers);
		return;
	}

	//
	// Calculate the space currently available in the buffer
	//
	head = ring_info->head;
	ttail = ring_info->tail;

	if(ttail > head)
	{
		freespace = ttail - head - 1;
	}
	else
	{
		freespace = RING_BUF_SIZE + ttail - head -1;
	}

	usedspace = RING_BUF_SIZE - freespace - 1;

	ASSERT(freespace <= RING_BUF_SIZE);
	ASSERT(usedspace <= RING_BUF_SIZE);
	ASSERT(ttail <= RING_BUF_SIZE);
	ASSERT(head <= RING_BUF_SIZE);

#ifndef __x86_64__
	//
	// If this is a socketcall system call, determine the correct event type
	// by parsing the arguments and patch event_type accordingly
	// A bit of explanation: most linux architectures don't have a separate
	// syscall for each of the socket functions (bind, connect...). Instead,
	// the socket functions are aggregated into a single syscall, called
	// socketcall. The first socketcall argument is the call type, while the
	// second argument contains a pointer to the arguments of the original
	// call. I guess this was done to reduce the number of syscalls...
	//
	if(id == __NR_socketcall)
	{
		enum ppm_event_type tet;
		tet = parse_socketcall(&args, regs);

		if(event_type == PPME_GENERIC_E)
		{
			event_type = tet;
		}
		else
		{
			event_type = tet + 1;
		}
	}
#endif

	ASSERT(event_type < PPM_EVENT_MAX);

	//
	// Determine how many arguments this event has
	//
	args.nargs = g_event_info[event_type].nparams;
	args.arg_data_offset = args.nargs * sizeof(uint16_t);

	//
	// Make sure we have enough space for the event header.
	// We need at least space for the header plus 16 bit per parameter for the lengths.
	//
	if(likely(freespace >= sizeof(struct ppm_evt_hdr) + args.arg_data_offset))
	{
		//
		// Populate the header
		//
		struct ppm_evt_hdr* hdr = (struct ppm_evt_hdr*)(ring->buffer + head);

#ifdef PPM_ENABLE_SENTINEL
		hdr->sentinel_begin = ring->nevents;
#endif
		hdr->ts = timespec_to_ns(&ts);
		hdr->tid = current->pid;
		hdr->type = event_type;

		//
		// Populate the parameters for the filler callback
		//
		args.buffer = ring->buffer + head + sizeof(struct ppm_evt_hdr);
#ifdef PPM_ENABLE_SENTINEL
		args.sentinel = ring->nevents;
#endif
		args.buffer_size = min(freespace, (uint32_t)(2 * PAGE_SIZE)) - sizeof(struct ppm_evt_hdr); // freespace is guaranteed to be bigger than sizeof(struct ppm_evt_hdr)
		args.event_type = event_type;
		args.regs = regs;
		args.syscall_id = id;
		args.curarg = 0;
		args.arg_data_size = args.buffer_size - args.arg_data_offset;
		args.nevents = ring->nevents;
		args.str_storage = ring->str_storage;

		//
		// Fire the filler callback
		//
		if(g_ppm_events[event_type].filler_callback == PPM_AUTOFILL)
		{
			//
			// This event is automatically filled. Hand it to f_sys_autofill.
			//
			cbres = f_sys_autofill(&args, &g_ppm_events[event_type]);
		}
		else
		{
			//
			// There's a callback function for this event
			//
			cbres = g_ppm_events[event_type].filler_callback(&args);
		}

		if(likely(cbres == PPM_SUCCESS))
		{
			//
			// Validate that the filler added the right number of parameters
			//
			if(likely(args.curarg == args.nargs))
			{
				event_size = sizeof(struct ppm_evt_hdr) + args.arg_data_offset;
				hdr->len = event_size;
				drop = 0;
			}
			else
			{
				printk(KERN_INFO "PPM: corrupted filler for event type %d (added %u args, should have added %u)\n",
				       event_type,
				       args.curarg,
				       args.nargs);
				ASSERT(0);
			}
		}
	}

	if(likely(!drop))
	{
		next = head + event_size;
		/*
				printk(KERN_INFO "%u) E:%u H:%x S:%x N:%x B:%p",
						smp_processor_id(),
						(uint32_t)event_type,
						head,
						event_size,
						next,
						args.buffer);

				*(ring->buffer + head + 5) = 0x33;
				*(ring->buffer + head + 6) = 0x44;

				if(event_size >=8)
				{
					printk(KERN_INFO "%.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x ",
							(uint32_t)*(unsigned char*)(ring->buffer + head),
							(uint32_t)*(unsigned char*)(ring->buffer + head + 1),
							(uint32_t)*(unsigned char*)(ring->buffer + head + 2),
							(uint32_t)*(unsigned char*)(ring->buffer + head + 3),
							(uint32_t)*(unsigned char*)(ring->buffer + head + 4),
							(uint32_t)*(unsigned char*)(ring->buffer + head + 5),
							(uint32_t)*(unsigned char*)(ring->buffer + head + 6),
							(uint32_t)*(unsigned char*)(ring->buffer + head + 7));
				}
		*/
		if(unlikely(next >= RING_BUF_SIZE))
		{
			//
			// If something has been written in the cushion space at the end of
			// the buffer, copy it to the beginning and wrap the head around.
			// Note, we don't check that the copy fits because we assume that
			// filler_callback failed if the space was not enough.
			//
			if(next > RING_BUF_SIZE)
			{
				memcpy(ring->buffer,
		                ring->buffer + RING_BUF_SIZE,
		                next - RING_BUF_SIZE);
			}

			next -= RING_BUF_SIZE;
		}

		//
		// Make sure all the memory has been written in real memory before
		// we update the head and the user space process (on another CPU)
		// can access the buffer.
		//
		smp_wmb();

		ring_info->head = next;

		++ring->nevents;
	}
	else
	{
		if(cbres == PPM_SUCCESS)
		{
			ASSERT(freespace < sizeof(struct ppm_evt_hdr) + args.arg_data_offset);
			ring_info->n_drops_buffer++;
		}
		else if(cbres == PPM_FAILURE_INVALID_USER_MEMORY)
		{
#ifdef _DEBUG
			printk(KERN_INFO "PPM: Invalid read from user for event %d\n", event_type);
#endif
			ring_info->n_drops_pf++;
		}
		else if(cbres == PPM_FAILURE_BUFFER_FULL)
		{
			ring_info->n_drops_buffer++;			
		}
		else
		{
			ASSERT(false);
		}
	}

#ifdef _DEBUG
	if(ts.tv_sec > ring->last_print_time.tv_sec + 1)
	{
		printk(KERN_INFO "PPM: CPU %d, util:%d%%, ev:%llu, dr_buf:%llu, dr_pf:%llu, pr:%llu\n",
		       smp_processor_id(),
		       (usedspace * 100) / RING_BUF_SIZE,
		       ring_info->n_evts,
		       ring_info->n_drops_buffer,
		       ring_info->n_drops_pf,
		       ring_info->n_preemptions);

		ring->last_print_time = ts;
	}
#endif

	atomic_dec(&ring->preempt_count);
	put_cpu_var(g_ring_buffers);

	return;
}

TRACEPOINT_PROBE(syscall_enter_probe, struct pt_regs *regs, long id)
{
	trace_enter();

	if(likely(id >= 0 && id < SYSCALL_TABLE_SIZE))
	{
		//
		// If this is a 32bit process running on a 64bit kernel (see the CONFIG_IA32_EMULATION
		// kernel flag), we skip its events.
		// XXX Decide what to do about this.
		//
		if(unlikely(test_tsk_thread_flag(current, TIF_IA32)))
		{
			return;		
		}

		if(g_syscall_table[id].used)
		{
			record_event(g_syscall_table[id].enter_event_type, regs, id);
		}
		else
		{
			record_event(PPME_GENERIC_E, regs, id);
		}
	}
}

TRACEPOINT_PROBE(syscall_exit_probe, struct pt_regs *regs, long ret)
{
	int id;

	trace_enter();

	id = syscall_get_nr(current, regs);

	if(likely(id >= 0 && id < SYSCALL_TABLE_SIZE))
	{
		//
		// If this is a 32bit process running on a 64bit kernel (see the CONFIG_IA32_EMULATION
		// kernel flag), we skip its events.
		// XXX Decide what to do about this.
		//
		if(unlikely(test_tsk_thread_flag(current, TIF_IA32)))
		{
			return;		
		}
		
		if(g_syscall_table[id].used)
		{
			record_event(g_syscall_table[id].exit_event_type, regs, id);
		}
		else
		{
			record_event(PPME_GENERIC_X, regs, id);
		}
	}
}

int __access_remote_vm(struct task_struct * t, struct mm_struct *mm, unsigned long addr,
                       void *buf, int len, int write);

/*
static void syscall_proceenter_probe(void *__data, struct pt_regs *regs, long ret)
{
	trace_enter();

	record_event(PPME_PROC_START, regs, -1);
}
*/

TRACEPOINT_PROBE(syscall_procexit_probe, struct pt_regs *regs, long ret)
{
	trace_enter();

	if(unlikely(current->flags & PF_KTHREAD))
	{
		//
		// We are not interested in kernel threads
		//
		return;
	}
	
	record_event(PPME_PROCEXIT_E, regs, -1);
}

#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>

TRACEPOINT_PROBE(netif_rx_probe, struct sk_buff *skb)
{
	/*
		struct iphdr *ih;
		struct udphdr *uh;
		ih = ip_hdr(skb);
		uh = udp_hdr(skb);

		printk(KERN_INFO "A %p-%p=%d:%u:%d(%d)->%d(%d) -- %d",
			uh,
			ih,
			(int)((void*)uh-(void*)ih),
			skb->len,
	//		(int)ih->version,
			(int)uh->source,
			(int)ntohs(uh->source),
			(int)uh->dest,
			(int)ntohs(uh->dest),
			(int)skb->skb_iif);
	*/
}

static struct ppm_ring_buffer_context* alloc_ring_buffer(struct ppm_ring_buffer_context** ring)
{
	unsigned int j;
	trace_enter();

	//
	// Allocate the ring descriptor
	//
	*ring = vmalloc(sizeof(struct ppm_ring_buffer_context));
	if(*ring == NULL)
	{
		printk(KERN_ERR "PPM: Error allocating ring memory\n");
		return NULL;
	}

	//
	// Allocate the string storage in the ring descriptor
	//
	(*ring)->str_storage = (char *) __get_free_page(GFP_USER);
	if(!(*ring)->str_storage)
	{
		printk(KERN_ERR "PPM: Error allocating the string storage\n");
		vfree(*ring);
		return NULL;
	}

	//
	// Allocate the buffer.
	// Note how we allocate 2 additional pages: they are used as additional overflow space for
	// the event data generation functions, so that they always operate on a contiguous buffer.
	//
	(*ring)->buffer = vmalloc(RING_BUF_SIZE + 2 * PAGE_SIZE);
	if((*ring)->buffer == NULL)
	{
		printk(KERN_ERR "PPM: Error allocating ring memory\n");
		free_page((unsigned long)(*ring)->str_storage);
		vfree(*ring);
		return NULL;
	}

	for(j = 0; j < RING_BUF_SIZE + 2 * PAGE_SIZE; j++)
	{
		(*ring)->buffer[j] = 0;
	}

	//
	// Allocate the buffer info structure
	//
	(*ring)->info = vmalloc(sizeof(struct ppm_ring_buffer_info));
	if((*ring)->info == NULL)
	{
		printk(KERN_ERR "PPM: Error allocating ring memory\n");
		vfree((void*)(*ring)->buffer);
		free_page((unsigned long)(*ring)->str_storage);
		vfree(*ring);
		return NULL;
	}

//    for(j = 0; j < (RING_BUF_SIZE / PAGE_SIZE + 1); j += PAGE_SIZE)
//    {
//		SetPageReserved(vmalloc_to_page(ring) + j);
//    }

	//
	// Initialize the buffer info structure
	//
	(*ring)->state = CS_STOPPED;
	(*ring)->info->head = 0;
	(*ring)->info->tail = 0;
	(*ring)->nevents = 0;
	(*ring)->info->n_evts = 0;
	(*ring)->info->n_drops_buffer = 0;
	(*ring)->info->n_drops_pf = 0;
	(*ring)->info->n_preemptions = 0;
	atomic_set(&(*ring)->preempt_count, 0);
	getnstimeofday(&(*ring)->last_print_time);

	printk(KERN_INFO "PPM: CPU buffer initialized, size=%d\n", RING_BUF_SIZE);

	return *ring;
}

static void free_ring_buffer(struct ppm_ring_buffer_context* ring)
{
	trace_enter();

	vfree(ring->info);
	vfree((void*)ring->buffer);
	free_page((unsigned long)ring->str_storage);
	vfree(ring);
}

// static int ppm_read_proc(char *page, char **start, off_t off, int count, int *eof, void *data)
// {
// 	int len = 0;
// 	int j;

// 	trace_enter();

// 	for(j = 0; j < NR_syscalls; ++j)
// 	{
// #if defined(__x86_64__)
// 		len += snprintf(page + len, count - len, "%ld\t%ld\n",
// 		                atomic64_read(&g_syscall_count[j].count),
// 		                atomic64_read(&g_syscall_count[j].count) ? (atomic64_read(&g_syscall_count[j].tot_time_ns) / atomic64_read(&g_syscall_count[j].count)) : 0);
// #else
// 		len += snprintf(page + len, count - len, "%lld\n",
// 		                atomic64_read(&g_syscall_count[j].count));
// #endif
// 	}

// 	*eof = 1;
// 	return len;
// }

int init_module(void)
{
	dev_t dev;
	unsigned int cpu;
	unsigned int num_cpus;
	int ret;
	int acrret = 0;
	int j;
	int n_created_devices = 0;
	struct device *device = NULL;

	printk(KERN_INFO "PPM: driver loading\n");

	//
	// Initialize the ring buffers array
	//
	num_cpus = 0;
	for_each_online_cpu(cpu)
	{
		per_cpu(g_ring_buffers, cpu) = NULL;
		++num_cpus;
	}

	for_each_online_cpu(cpu)
	{
		printk(KERN_INFO "PPM: initializing ring buffer for CPU %u\n", cpu);

		alloc_ring_buffer(&per_cpu(g_ring_buffers, cpu));
		if(per_cpu(g_ring_buffers, cpu) == NULL)
		{
			printk(KERN_ERR "PPM: can't initialize the ring buffer for CPU %u\n", cpu);
			ret = -ENOMEM;
			goto init_module_err;
		}
	}

	//
	// Initialize the user I/O
	//
	acrret = alloc_chrdev_region(&dev, 0, num_cpus, PPM_DEVICE_NAME);
	if(acrret < 0)
	{
		printk(KERN_ERR "PPM: could not allocate major number for %s\n", PPM_DEVICE_NAME);
		ret = -ENOMEM;
		goto init_module_err;
	}

	g_ppm_class = class_create(THIS_MODULE, PPM_DEVICE_NAME);
	if(IS_ERR(g_ppm_class))
	{
		printk(KERN_ERR "PPM: can't allocate device class\n");
		ret = -EFAULT;
		goto init_module_err;
	}

	g_ppm_major = MAJOR(dev);
	g_ppm_numdevs = num_cpus;
	g_ppm_devs = kmalloc(g_ppm_numdevs * sizeof (struct ppm_device), GFP_KERNEL);
	if(!g_ppm_devs)
	{
		ret = -ENOMEM;
		goto init_module_err;
		printk(KERN_ERR "PPM: can't allocate devices\n");
	}

	//
	// We create a unique user level device for each of the ring buffers
	//
	for(j = 0; j < g_ppm_numdevs; ++j)
	{
		cdev_init(&g_ppm_devs[j].cdev, &g_ppm_fops);
		g_ppm_devs[j].dev = MKDEV(g_ppm_major, j);

		if(cdev_add(&g_ppm_devs[j].cdev, g_ppm_devs[j].dev, 1) < 0)
		{
			printk(KERN_ERR "PPM: could not allocate chrdev for %s\n", PPM_DEVICE_NAME);
			ret = -EFAULT;
			goto init_module_err;
		}

		device = device_create(g_ppm_class, NULL, // no parent device
		                       g_ppm_devs[j].dev,
		                       NULL, // no additional data
		                       PPM_DEVICE_NAME "%d",
		                       j);

		if(IS_ERR(device))
		{
			printk(KERN_ERR "PPM: error creating the device for  %s\n", PPM_DEVICE_NAME);
			cdev_del(&g_ppm_devs[j].cdev);
			ret = -EFAULT;
			goto init_module_err;
		}

		init_waitqueue_head(&g_ppm_devs[j].read_queue);
		n_created_devices++;
	}

	// create_proc_read_entry(PPM_DEVICE_NAME, 0, NULL, ppm_read_proc, NULL);

	//
	// All ok. Final initalizations.
	//
	atomic_set(&g_open_count, 0);

	return 0;

init_module_err:
	for_each_online_cpu(cpu)
	{
		if(per_cpu(g_ring_buffers, cpu) != NULL)
		{
			free_ring_buffer(per_cpu(g_ring_buffers, cpu));
		}
	}

	// remove_proc_entry(PPM_DEVICE_NAME, NULL);

	for(j = 0; j < n_created_devices; ++j)
	{
		device_destroy(g_ppm_class, g_ppm_devs[j].dev);
		cdev_del(&g_ppm_devs[j].cdev);
	}

	if(g_ppm_class)
	{
		class_destroy(g_ppm_class);
	}

	if(acrret == 0)
	{
		unregister_chrdev_region(dev, g_ppm_numdevs);
	}

	if(g_ppm_devs)
	{
		kfree(g_ppm_devs);
	}

	return ret;
}

void cleanup_module(void)
{
	int j;
	int cpu;

	printk(KERN_INFO "PPM: driver unloading\n");

	// remove_proc_entry(PPM_DEVICE_NAME, NULL);

	for_each_online_cpu(cpu)
	{
		free_ring_buffer(per_cpu(g_ring_buffers, cpu));
	}

	for(j = 0; j < g_ppm_numdevs; ++j)
	{
		device_destroy(g_ppm_class, g_ppm_devs[j].dev);
		cdev_del(&g_ppm_devs[j].cdev);
	}

	if(g_ppm_class)
	{
		class_destroy(g_ppm_class);
	}

	unregister_chrdev_region(MKDEV(g_ppm_major, 0), g_ppm_numdevs);

	kfree(g_ppm_devs);
}
