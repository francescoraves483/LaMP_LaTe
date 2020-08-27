#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include "dup_list.h"

struct dupStoreListNode {
	unsigned int seqNo; // Acting as search key

	unsigned int occupied;

	struct dupStoreListNode *next;
};

struct _dupStoreList {
	struct dupStoreListNode *array;
	int size;
	int8_t past_list_pos; // = -1 if uninitialized, = 0 if past list is between 0 and DSL->size-1, = 1 if past list is between DSL->size and DSL->size*2-1
};

dupStoreList dupSL_init(int size) {
	dupStoreList DSL;

	if(size<=0) {
		return NULL;
	}

	DSL=malloc(sizeof(struct _dupStoreList));
	if(!CHECK_DSL_NULL(DSL)) {
		DSL->size=size;

		DSL->array=(struct dupStoreListNode *) malloc(DSL->size*2*sizeof(struct dupStoreListNode));

		if(!DSL->array) {
			free(DSL);
			return NULL;
		}
	}

	DSL->past_list_pos=-1;

	return DSL;
}

int dupSL_insertandcheck(dupStoreList DSL, unsigned int seqNo) {
	unsigned int seqNoHash = seqNo % DSL->size;
	unsigned int pastSeqNoHash;
	unsigned int found=DSL_NOTFOUND;

	if(DSL->size==0) {
		return DSL_ZEROSIZE;
	}

	if(DSL->past_list_pos!=-1) {
		if(DSL->past_list_pos==1) {
			pastSeqNoHash=seqNoHash+DSL->size;
		} else {
			pastSeqNoHash=seqNoHash;
		}

		// Look into the past part of the list
		if(DSL->array[pastSeqNoHash].occupied==0) {
			found=DSL_NOTFOUND;
		} else if(DSL->array[pastSeqNoHash].occupied==1 && seqNo==DSL->array[pastSeqNoHash].seqNo) {
			found=DSL_FOUND;
		}

		if(found==DSL_FOUND) {
			return found;
		}
	}

	// Look into the current part of the list
	if(DSL->past_list_pos==0) {
		seqNoHash+=DSL->size;
	}

	// Check if the array data at the current hash (which is simply seqNo % DSL->size) is free
	if(DSL->array[seqNoHash].occupied==0) {
		// Store the new data here
		DSL->array[seqNoHash].occupied=1;
		DSL->array[seqNoHash].seqNo=seqNo;
		return DSL_NOTFOUND;
	}

	// Look at the current array position, is the right data there?
	// If yes, the data is there -> return DSL_FOUND
	if(DSL->array[seqNoHash].occupied==1 && seqNo==DSL->array[seqNoHash].seqNo) {
		found=DSL_FOUND;
	} else {
		int newSeqNoHash = DSL->past_list_pos==0 ? seqNoHash-DSL->size : seqNoHash+DSL->size;
		// Switch current <-> past list, as the current list is full
		dupSL_reset(DSL);

		// Now the position corresponding to seqNoHash (which is now a past list index) should be free
		if(DSL->array[newSeqNoHash].occupied==0) {
			DSL->array[newSeqNoHash].occupied=1;
			DSL->array[newSeqNoHash].seqNo=seqNo;
			found=DSL_NOTFOUND;
		} else {
			found=DSL_POSCONFLICT; // An error occurred if the position is still not free
		}
	}

	return found;
}

void dupSL_reset(dupStoreList DSL) {
	int start_i = DSL->past_list_pos==0 ? 0 : DSL->size;
	int stop_i = DSL->past_list_pos==0 ? DSL->size : DSL->size*2;

	// Try to reset all the sub-lists
	for(int i=start_i;i<stop_i;i++) {
		DSL->array[i].occupied=0;
	}

	// Update the position of the current list, which will become the new past list
	if(DSL->past_list_pos==0) {
		DSL->past_list_pos=1;
	} else {
		DSL->past_list_pos=0;
	}
}

void dupSL_free(dupStoreList DSL) {
	if(!CHECK_DSL_NULL(DSL)) {
		if(!CHECK_DSL_NULL(DSL->array)) {
			free(DSL->array);
		}
		free(DSL);
	}
}