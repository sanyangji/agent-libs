///////////////////////////////////////////////////////////////////////
// Driver output definitions
///////////////////////////////////////////////////////////////////////

//
// Driver Chattiness
//
#define OUTPUT_VERBOSE 4
#define OUTPUT_INFO 2
#define OUTPUT_ERRORS 1
#define OUTPUT_NONE 0

#define OUTPUT_LEVEL OUTPUT_INFO

//
// Our Own ASSERT implementation, so we can easily switch among BUG_ON, WARN_ON and nothing
//
#ifdef _DEBUG
#define ASSERT(expr) WARN_ON(!(expr))
#else
#define ASSERT(expr)
#endif

//
// Tracing and debug printing
//

#if (OUTPUT_LEVEL >= OUTPUT_VERBOSE)
#define dbgprint(a) printk(KERN_INFO a "\n")
#define trace_enter() printk(KERN_INFO "> %s\n", __FUNCTION__)
#define trace_exit() printk(KERN_INFO "< %s\n", __FUNCTION__)
#else
#define dbgprint(a)
#define trace_exit()
#define trace_enter()
#endif

///////////////////////////////////////////////////////////////////////
// Global defines
///////////////////////////////////////////////////////////////////////
#define RW_SNAPLEN 80

///////////////////////////////////////////////////////////////////////
// Global enums
///////////////////////////////////////////////////////////////////////
typedef enum ppm_capture_state
{
	CS_STOPPED,		// Not capturing. Either uninitialized or closed.
	CS_STARTED,		// Capturing.
	CS_INACTIVE,	// Not Capturing but active, returning the packets in the buffer to the user.
}ppm_capture_state;

enum syscall_used_flag
{
	UF_NONE = 0,
	UF_NORMAL_MODE = (1 << 0),
	UF_DROPPING_MODE = (1 << 1),
};

///////////////////////////////////////////////////////////////////////
// Global structs
///////////////////////////////////////////////////////////////////////
struct syscall_evt_pair
{
	int used;
	enum ppm_event_type enter_event_type;
	enum ppm_event_type exit_event_type;
};

struct ppm_device
{
	dev_t dev;
	struct cdev cdev;
	wait_queue_head_t read_queue;
};

#define STR_STORAGE_SIZE PAGE_SIZE

//
// The ring descriptor.
// We have one of these for each CPU.
//
struct ppm_ring_buffer_context
{
	ppm_capture_state state;
	struct ppm_ring_buffer_info* info;
	char* buffer;
	struct timespec last_print_time;
	uint32_t nevents;
	atomic_t preempt_count;
	char* str_storage;	// String storage. Size is one page.
};

///////////////////////////////////////////////////////////////////////
// Global functions
///////////////////////////////////////////////////////////////////////
unsigned long ppm_copy_from_user(void *to, const void __user *from, unsigned long n);
long ppm_strncpy_from_user(char *to, const char __user *from, unsigned long n);

///////////////////////////////////////////////////////////////////////
// Global tables
///////////////////////////////////////////////////////////////////////
#define SYSCALL_TABLE_SIZE 512

extern const struct syscall_evt_pair g_syscall_table[];
extern const struct ppm_event_info g_event_info[];
extern const struct ppm_syscall_desc g_syscall_info_table[];
extern const ppm_syscall_code g_syscall_code_routing_table[];
