#ifndef LATENCYTEST_MATHUTILS_H_INCLUDED
#define LATENCYTEST_MATHUTILS_H_INCLUDED

#include "named_enums.h"

// Defined as "named enum" (see named_enums.h)
#define RANDTYPES(RANDTYPE) \
	RANDTYPE(NON_RAND,=0) \
	RANDTYPE(RAND_PSEUDOUNIFORM,=1) \
	RANDTYPE(RAND_UNIFORM,=2) \
	RANDTYPE(RAND_EXPONENTIAL,=3) \
	RANDTYPE(RAND_NORMAL,=4)

NAMED_ENUM_DECLARE(rand_distribution_t,RANDTYPES);

int rand_pseudouniform(int min,int max);
int rand_uniform(int min,int max);
double rand_exponential(double mean);
double rand_gaussian(double mean,double stddev);

#endif