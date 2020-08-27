#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include "carbon_dup_list.h"

struct carbonDupStoreListNode {
	unsigned int seqNo; // Acting as search key

	unsigned int occupied;

	struct carbonDupStoreListNode *next;
};

struct _carbonDupStoreList {
	struct carbonDupStoreListNode **heads;
	int size;
	int8_t past_list_pos; // = -1 if uninitialized, = 0 if past list is between 0 and CDSL->size-1, = 1 if past list is between CDSL->size and CDSL->size*2-1
};

carbonDupStoreList carbonDupSL_init(int size) {
	carbonDupStoreList CDSL;
	int i=0;

	if(size<=0) {
		return NULL;
	}

	CDSL=malloc(sizeof(struct _carbonDupStoreList));
	if(!CHECK_CDSL_NULL(CDSL)) {
		CDSL->size=size;

		CDSL->heads=(struct carbonDupStoreListNode **) malloc(CDSL->size*2*sizeof(struct carbonDupStoreListNode));

		if(!CDSL->heads) {
			free(CDSL);
			return NULL;
		}

		for(i=0;i<CDSL->size*2;i++) {
			// Allocate first node (i.e. the 'head' of each sub-list), which is always allocated (it is freed only when calling carbonDupSL_free())
			CDSL->heads[i]=(struct carbonDupStoreListNode *) malloc(sizeof(struct carbonDupStoreListNode));
			if(CDSL->heads[i]!=NULL) {
				CDSL->heads[i]->next=NULL;
				CDSL->heads[i]->occupied=0;
			} else {
				break;
			}
		}

		if(i!=CDSL->size*2) {
			for(int j=0;j<i;j++) {
				free(CDSL->heads[i]);
			}

			free(CDSL->heads);
			free(CDSL);
			CDSL=NULL;
		}
	}

	CDSL->past_list_pos=-1;

	return CDSL;
}

int carbonDupSL_insertandcheck(carbonDupStoreList CDSL, unsigned int seqNo) {
	unsigned int seqNoHash = seqNo % CDSL->size;
	unsigned int pastSeqNoHash;
	struct carbonDupStoreListNode *nptr, *nptr_prev, *CDSLN;
	unsigned int found=CDSL_NOTFOUND;

	if(CDSL->size==0) {
		return CDSL_ZEROSIZE;
	}

	if(CDSL->past_list_pos!=-1) {
		if(CDSL->past_list_pos==1) {
			pastSeqNoHash=seqNoHash+CDSL->size;
		} else {
			pastSeqNoHash=seqNoHash;
		}

		// Look into the past part of the list
		if(CDSL->heads[pastSeqNoHash]->occupied==0) {
			found=CDSL_NOTFOUND;
		} else {
			if(CDSL->heads[pastSeqNoHash]->occupied==1 && seqNo==CDSL->heads[pastSeqNoHash]->seqNo) {
				found=CDSL_FOUND;
			} else {
				for(nptr=CDSL->heads[pastSeqNoHash]->next;nptr!=NULL && found==CDSL_NOTFOUND;nptr=nptr->next) {
					if(seqNo==nptr->seqNo) {
						found=CDSL_FOUND;
					}
				}
			}
		}

		if(found==CDSL_FOUND) {
			return found;
		}
	}

	// Look into the current part of the list
	if(CDSL->past_list_pos==0) {
		seqNoHash+=CDSL->size;
	}

	// Check if the head at the current hash is free
	if(CDSL->heads[seqNoHash]->occupied==0) {
		// Store the new data here
		CDSL->heads[seqNoHash]->seqNo=seqNo;
		CDSL->heads[seqNoHash]->occupied=1;
		return CDSL_NOTFOUND;
	}

	// Look at the head, is the right data there?
	// If yes, the data is there -> return CDSL_FOUND
	if(CDSL->heads[seqNoHash]->occupied==1 && seqNo==CDSL->heads[seqNoHash]->seqNo) {
		found=CDSL_FOUND;
	} else {
		// Scan list
		nptr_prev=CDSL->heads[seqNoHash];
		for(nptr=CDSL->heads[seqNoHash]->next;nptr!=NULL && found==CDSL_NOTFOUND;nptr=nptr->next) {
			nptr_prev=nptr;
			if(seqNo==nptr->seqNo) {
				// Node found!
				found=CDSL_FOUND;
			}
		}

		if(found==CDSL_NOTFOUND) {
			// Allocate a new element in the list
			CDSLN=(struct carbonDupStoreListNode *) malloc(sizeof(struct carbonDupStoreListNode));

			if(CDSLN==NULL) {
				return CDSL_NOMEM;
			}

			CDSLN->seqNo=seqNo;
			CDSLN->occupied=1;
			CDSLN->next=NULL;
			nptr_prev->next=CDSLN;
		}
	}

	return found;
}

void carbonDupSL_reset(carbonDupStoreList CDSL) {
	struct carbonDupStoreListNode *nptr, *nnptr;
	int start_i = CDSL->past_list_pos==0 ? 0 : CDSL->size;
	int stop_i = CDSL->past_list_pos==0 ? CDSL->size : CDSL->size*2;

	// Try to reset all the sub-lists
	for(int i=start_i;i<stop_i;i++) {
		// Scan sub-list (the loop will not be executed if the sub-list head only is present)
		for(nptr=CDSL->heads[i]->next;nptr!=NULL;nptr=nnptr) {
			nnptr=nptr->next;
			free(nptr);
		}

		CDSL->heads[i]->next=NULL;

		// Un-occupy the head
		if(CDSL->heads[i]->occupied==1) {
			CDSL->heads[i]->occupied=0;
		}
	}

	// Update the position of the current list, which will become the new past list
	if(CDSL->past_list_pos==0) {
		CDSL->past_list_pos=1;
	} else {
		CDSL->past_list_pos=0;
	}
}

void carbonDupSL_free(carbonDupStoreList CDSL) {
	struct carbonDupStoreListNode *nptr, *nnptr;

	if(!CHECK_CDSL_NULL(CDSL)) {
		if(!CHECK_CDSL_NULL(CDSL->heads)) {
			for(int i=0;i<CDSL->size;i++) {
				for(nptr=CDSL->heads[i];nptr!=NULL;nptr=nnptr) {
					nnptr=nptr->next;
					free(nptr);
				}
			}
			free(CDSL->heads);
		}
		free(CDSL);
	}
}