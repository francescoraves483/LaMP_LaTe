#ifndef LATENCYTEST_DUPLIST_H_INCLUDED
#define LATENCYTEST_DUPLIST_H_INCLUDED

#define CHECK_DSL_NULL(SL) (SL==NULL)

// dupStoreList errors
#define DSL_ZEROSIZE    	4
#define DSL_NOTFOUND		3
#define DSL_EMPTY 			2
#define DSL_POSCONFLICT 	1
#define DSL_NOERR 			0
#define DSL_FOUND  			0

typedef struct _dupStoreList *dupStoreList;
dupStoreList dupSL_init(int size);
int dupSL_insertandcheck(dupStoreList DSL, unsigned int seqNo);
void dupSL_reset(dupStoreList DSL);
void dupSL_free(dupStoreList DSL);


#endif