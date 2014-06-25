#ifndef __LISTQSORT_HPP__
#define __LISTQSORT_HPP__

template<typename T> void listqsort(T *&begin, T *end=NULL) {
	if(begin!=end) {
		//split input list in greater/less than pivot (i.e., *begin)
		T *gtfront = NULL, *ltfront = NULL;
		T **gtback = &gtfront, **ltback = &ltfront;
		for(T *current=begin->next; current!=end; current=current->next)
			if(*current>*begin) {
				*gtback = current;
				gtback = &current->next;
			} else {
				*ltback = current;
				ltback = &current->next;
			}
		//set end for lists
		*gtback = end,
		*ltback = begin;
		//recursive step
		listqsort(ltfront, *ltback),
		listqsort(gtfront, *gtback);
		//concat resulting lists
		begin->next = gtfront;
		begin = ltfront;
	}
}

#endif