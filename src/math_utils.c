#include "math_utils.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

NAMED_ENUM_DEFINE_FCNS(rand_distribution_t,RANDTYPES);

/* 	
	This function simply returns an integer random value
	between min and max, using rand().
*/
int rand_pseudouniform(int min,int max) {
	return min+rand()%(max-min+1);
}

/* 	
	This function returns an integer random value
	between min and max, using rand(), and trying
	to avoid any modulo bias in the result.
	May be slower to execute than rand_pseudouniform().
*/
int rand_uniform(int min,int max) {
	int u;
	int n=max-min+1;

	do {
		u=rand();
	} while(u>=(RAND_MAX-RAND_MAX%n));

	return min+rand()%n;
}

/* 	
	This function should return an exponentially distributed double number 
	given the mean of the exponential distribution (mean=1/lambda).
	It is using the inverse probability integral transform to generate an 
	exponential random number starting from a uniform distribution, given 
	by rand().

	Basically, x = -log(1-u)/lambda = -log(1-u)*mean, where u is uniformely 
	distributed between 0 and 1 (thanks to the division by RAND_MAX+1).
*/
double rand_exponential(double mean) {
    return -log(1-(rand()/((double)RAND_MAX+1.0)))*mean;
}

/* 	
	This function should return an random double number accoring to the 
	normal distribution.
	It is using a standard implementation of the Marsaglia polar method.
*/
double rand_gaussian(double mean,double stddev) {
	static double x,y,s;
	static uint8_t has_prev_var=0;
	static double prev_var=-1.0;

	// This is done because, each time, the method returns a pair of normal
	// random variables (s*x and s*y), which are independent.
	// Thus, we can return one variable in the first function call and the other
	// in the next one, avoiding a call to rand() for each call.
	if(has_prev_var!=0) {
		has_prev_var=0;
		return mean+stddev*prev_var;
	}

	do {
		x=(rand()/((double)RAND_MAX))*2.0-1.0;
		y=(rand()/((double)RAND_MAX))*2.0-1.0;
		s=x*x+y*y;
	} while(s>=1.0 || s<=0);

	s=sqrt(-2.0*log(s)/s);

	prev_var=y*s;
	has_prev_var=1;

	return mean+stddev*x*s;
}