#ifndef RLIMIT_H
#define RLIMIT_H

#include   <stdio.h>

#define RLIMIT_CPU      0   /* limit on CPU time per process */
#define RLIMIT_FSIZE    1   /* limit on file size */
#define RLIMIT_DATA     2   /* limit on data segment size */
#define RLIMIT_STACK    3   /* limit on process stack size */
#define RLIMIT_CORE     4   /* limit on size of core dump file */
#define RLIMIT_NOFILE   5   /* limit on number of open files */
#define RLIMIT_AS       6   /* limit on process total address space size */
#define RLIMIT_VMEM     RLIMIT_AS
#define RLIM_NLIMITS    7


/*
 * process resource limits definitions
 */

struct rlimit {
//        LARGE_INTEGER  rlim_cur;
//        LARGE_INTEGER  rlim_max;
          __int64    rlim_cur;
          __int64    rlim_max;
};

typedef struct rlimit  rlimit_t;

/*
 * Prototypes
 */
int getrlimit(int resource, struct rlimit *);
int setrlimit(int resource, const struct rlimit *);

size_t rfwrite( const void *buffer, size_t size, size_t count, FILE *stream );
int _rwrite( int handle, const void *buffer, unsigned int count );

#endif //RLIMIT_H