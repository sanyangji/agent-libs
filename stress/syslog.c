#include <syslog.h>
     
int main()
{
	setlogmask (LOG_UPTO (LOG_NOTICE));

	openlog ("exampleprog", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

	syslog (LOG_NOTICE, "This is a notice message");
	syslog (LOG_NOTICE, "This is a notice message again");
	syslog (LOG_INFO, "A tree falls in a forest");

	closelog ();
}
