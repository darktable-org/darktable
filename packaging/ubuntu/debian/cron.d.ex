#
# Regular cron jobs for the darktable package
#
0 4	* * *	root	[ -x /usr/bin/darktable_maintenance ] && /usr/bin/darktable_maintenance
