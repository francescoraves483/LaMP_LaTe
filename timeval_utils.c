#include "timeval_utils.h"
#include <stdlib.h>

struct timeValStoreListNode {
	struct timeval txStamp;
	unsigned int seqNo; // Acting as search key

	unsigned int occupied;

	struct timeValStoreListNode *next;
};

struct _timevalStoreList {
	struct timeValStoreListNode *head;
};

timevalStoreList timevalSL_init(void) {
	timevalStoreList SL;

	SL=malloc(sizeof(struct _timevalStoreList));
	if(!CHECK_SL_NULL(SL)) {
		// Allocate first node, which is always allocated (it is freed only when calling timevalSL_free())
		SL->head=(struct timeValStoreListNode *) malloc(sizeof(struct timeValStoreListNode));
		if(SL->head!=NULL) {
			SL->head->next=NULL;
			SL->head->occupied=0;
		} else {
			free(SL);
			SL=NULL;
		}
	}

	return SL;
}

int timevalSL_insert(timevalStoreList SL, unsigned int seqNo, struct timeval stamp) {
	struct timeValStoreListNode *SLN;

	// Check if the head is free
	if(SL->head->occupied==0) {
		// Store the new data here
		SL->head->seqNo=seqNo;
		SL->head->txStamp=stamp;
		SL->head->occupied=1;
	} else {
		// If the head is occupied, allocate a new head
		SLN=(struct timeValStoreListNode *) malloc(sizeof(struct timeValStoreListNode));

		if(SLN==NULL) {
			return SL_NOMEM;
		}

		SLN->seqNo=seqNo;
		SLN->txStamp=stamp;
		SLN->occupied=1;
		SLN->next=SL->head;

		SL->head=SLN;
	}

	return SL_NOERR;
}

int timevalSL_gather(timevalStoreList SL, unsigned int seqNo, struct timeval *stamp) {
	struct timeValStoreListNode *nptr, *nptr_prev;
	unsigned int found=0;

	if(SL->head->next==NULL && SL->head->occupied==0) {
		return SL_EMPTY;
	}

	// Look at the head, is the right data there?
	// If yes, extract the data and simply un-occupy the head
	if(SL->head->occupied==1 && seqNo==SL->head->seqNo) {
		*stamp=SL->head->txStamp;

		SL->head->occupied=0;
	} else {
		// Scan list
		nptr_prev=SL->head;
		for(nptr=SL->head->next;nptr!=NULL && !found;nptr=nptr->next) {
			// When the element is found, rearrange the list and put the node in the trash nodes arrays
			if(seqNo==nptr->seqNo) {
				// Node found!
				*stamp=nptr->txStamp;

				nptr_prev->next=nptr->next;
				found=1;

				// Free the node
				free(nptr);
			}

			nptr_prev=nptr;
		}

		// If the element is not found, return an error
		if(!found) {
			return SL_NOTFOUND;
		}
	}

	return SL_NOERR;
}

void timevalSL_free(timevalStoreList SL) {
	struct timeValStoreListNode *nptr, *nnptr;

	if(!CHECK_SL_NULL(SL)) {
		for(nptr=SL->head;nptr!=NULL;nptr=nnptr) {
			nnptr=nptr->next;
			free(nptr);
		}

		free(SL);
	}
}