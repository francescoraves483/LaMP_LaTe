#ifndef LATENCYTEST_CARBONDUPLIST_H_INCLUDED
#define LATENCYTEST_CARBONDUPLIST_H_INCLUDED

#define CHECK_CDSL_NULL(SL) (SL==NULL)

// carbonDupStoreList errors
#define CDSL_ZEROSIZE    	4
#define CDSL_NOTFOUND		3
#define CDSL_EMPTY 			2
#define CDSL_NOMEM		 	1
#define CDSL_NOERR 			0
#define CDSL_FOUND  		0

typedef struct _carbonDupStoreList *carbonDupStoreList;
carbonDupStoreList carbonDupSL_init(int size);
int carbonDupSL_insertandcheck(carbonDupStoreList CDSL, unsigned int seqNo);
void carbonDupSL_reset(carbonDupStoreList CDSL);
void carbonDupSL_free(carbonDupStoreList CDSL);

#endif