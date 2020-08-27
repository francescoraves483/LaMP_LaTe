#ifndef LATENCYTEST_TIMEVALUTILS_H_INCLUDED
#define LATENCYTEST_TIMEVALUTILS_H_INCLUDED

#include <sys/time.h>

// "__attribute__((unused))" is added just to tell the clang compiler not to issue a warning
// for an unused 'static inline' (which is actually used in multiple modules)
static inline int timevalSub(struct timeval *in, struct timeval *out) __attribute__((unused));

// timevalStoreList errors
#define SL_NOTFOUND	3
#define SL_EMPTY 	2
#define SL_NOMEM 	1
#define SL_NOERR 	0

#define NULL_SL NULL
#define CHECK_SL_NULL(SL) (SL==NULL)

typedef struct _timevalStoreList *timevalStoreList;
timevalStoreList timevalSL_init();
int timevalSL_insert(timevalStoreList SL, unsigned int seqNo, struct timeval stamp);
int timevalSL_gather(timevalStoreList SL, unsigned int seqNo, struct timeval *stamp);
void timevalSL_free(timevalStoreList SL);

// This inline function will perform op2 = op2 - op1, leveraging on the timersub() macro
// The 'op1' timeval structure is always left unmodified
static inline int timevalSub(struct timeval *op1,struct timeval *op2) {
	if(op2->tv_sec<op1->tv_sec || (op2->tv_sec==op1->tv_sec && op2->tv_usec<op1->tv_usec)) {
		timersub(op1,op2,op2);
		return 1;
	} else {
		timersub(op2,op1,op2);
		return 0;
	}
}

#endif