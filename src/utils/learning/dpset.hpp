#ifndef __DPSET_HPP__
#define __DPSET_HPP__

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath> // NAN, isnan()
#include <omp.h> // omp_get_thread_num()

#ifdef SHOWTIMER
#include <sys/stat.h>
double filesize(const char *filename) {
	struct stat st;
	stat(filename, &st);
	return st.st_size/1048576.0;
}
#endif

// undf stands for "not defined" and is used for representing missing values in the feature matrix
#define undf NAN
#define isundf(x) isnan(x)

#include "utils/trie.hpp" // trie data structure
#include "utils/transpose.hpp" // traspose matrix
#include "utils/strutils.hpp" // split string
#include "utils/cpuinfo.hpp" // info from /proc/cpuinfo
#include "utils/bitarray.hpp" // bit array implementation
#include "utils/radix.hpp" // sorters
#include "utils/listqsort.hpp" // sorter for linked lists

#define SKIP_DPDESCRIPTIONS //comment to store dp descriptions
#define PRESERVE_DPFILEORDER //uncomment to store datapoints in the same order as in the input file. NOTE dplist.push() is not yet efficient, i.e. O(|dplist|), but dplist are usually short
#define INIT_NOFEATURES 50 //>0

struct rnklst {
	rnklst(unsigned int size, float *labels, char const* id) : size(size), labels(labels), id(id) {}
	const unsigned int size;
	float *labels;
	char const* id;
};

class dp { //each dp is related to a line read from file
	public:
		dp(const float label, const unsigned int nline, const unsigned int initsize=1) :
			maxsize(initsize>0?initsize:1),
			maxfid(0),
			nline(nline),
			label(label),
			features(NULL),
			#ifndef SKIP_DPDESCRIPTIONS
			description(NULL),
			#endif
			next(NULL) {
			features = (float*)malloc(sizeof(float)*maxsize);
			features[0] = undf;
		}
		void ins_feature(const unsigned int fid, const float fval) {
			if(fid>=maxsize) {
				maxsize = 2*fid+1;
				features = (float*)realloc(features, sizeof(float)*maxsize);
			}
			for(unsigned int i=maxfid+1; i<fid; features[i++]=undf);
			maxfid = fid>maxfid ? fid : maxfid,
			features[fid] = fval;
		}
		float *get_resizedfeatures(const unsigned int size) {
			if(size>=maxsize and size!=0)
				features = (float*)realloc(features, sizeof(float)*size);
			for(unsigned int i=maxfid+1; i<size; features[i++]=undf);
			return features;
		}
		float get_label() const {
			return label;
		}
		#ifndef SKIP_DPDESCRIPTIONS
		void set_description(char *str) {
			description = str;
		}
		char *get_description() const {
			return description;
		}
		#endif
	private:
		unsigned int maxsize, maxfid, nline;
		float label, *features;
		#ifndef SKIP_DPDESCRIPTIONS
		char *description;
		#endif
		dp *next;
	friend class dplist;
	#ifdef PRESERVE_DPFILEORDER
	friend bool operator> (dp &left, dp &right) { return left.nline>right.nline; };
	friend void listqsort<dp>(dp *&begin, dp *end);
	#endif
};

//dplist collects datapoints having the same id
class dplist {
	public:
		dplist(const char *key) : head(NULL), size(0), rid(strdup(key)) {}
		void push(dp* x) {
			x->next = head;
			head = x,
			++size;
		}
		void pop() {
			dp* tmp = head;
			head = head->next;
			delete tmp;
		}
		dp *front() const {
			return head;
		}
		unsigned int get_size() const {
			return size;
		}
		char *get_rid() const {
			return rid;
		}
		#ifdef PRESERVE_DPFILEORDER
		void sort_bynline() {
			listqsort<dp>(head);
		}
		#endif
	private:
		dp *head;
		unsigned int size;
		char *rid;
};

class dpset {
	public:
		dpset(const char *filename) {
			FILE *f = fopen(filename, "r");
			if(f) {
				#ifdef SHOWTIMER
				double readingtimer = omp_get_wtime();
				#endif
				unsigned int maxfid = INIT_NOFEATURES-1;
				unsigned int linecounter = 0;
				unsigned int th_ndps[NPROCESSORS] = {0};
				bitarray th_usedfid[NPROCESSORS];
				trie<dplist> rltrie;
				#pragma omp parallel num_threads(NPROCESSORS) shared(maxfid, linecounter)
				while(not feof(f)) {
					ssize_t nread;
					size_t linelength = 0;
					char *line = NULL;
					unsigned int nline = 0;
					//lines are read one-at-a-time by threads
					#pragma omp critical
					{ nread = getline(&line, &linelength, f), nline = ++linecounter; }
					//if something is wrong with getline() or line is empty, skip to the next
					if(nread<=0) { free(line); continue; }
					char *key = NULL, *token = NULL, *pch = line;
					//skip initial spaces
					while(ISSPC(*pch) && *pch!='\0') ++pch;
					//skip comment line
					if(*pch=='#') { free(line); continue; }
					//each thread get its id to access th_usedfid[], th_ndps[]
					const int ith = omp_get_thread_num();
					//read label (label is a mandatory field)
					if(ISEMPTY(token=read_token(pch))) exit(2);
					//create a new dp for storing the max number of features seen till now
					dp *newdp = new dp(atof(token), nline, maxfid+1);
					//read id (id is a mandatory field)
					if(ISEMPTY(key=read_token(pch))) exit(3);
					//read a sequence of features, namely (fid,fval) pairs, then the ending description
					while(!ISEMPTY(token=read_token(pch,'#')))
						if(*token=='#') {
							#ifndef SKIP_DPDESCRIPTIONS
							newdp->set_description(strdup(++token));
							#endif
							*pch = '\0';
						} else {
							//read a feature (id,val) from token
							unsigned int fid = 0;
							float fval = undf;
							if(sscanf(token, "%u:%f", &fid, &fval)!=2) exit(4);
							//add feature to the current dp
							newdp->ins_feature(fid, fval),
							//update used featureids
							th_usedfid[ith].set_up(fid),
							//update maxfid (it should be "atomically" managed but its consistency is not a problem)
							maxfid = fid>maxfid ? fid : maxfid;
						}
					//store current sample in trie
					#pragma omp critical
					{ rltrie.insert(key)->push(newdp); }
					//update thread dp counter
					++th_ndps[ith],
					//free mem
					free(line);
				}
				//close input file
				fclose(f);
				#ifdef SHOWTIMER
				readingtimer = omp_get_wtime()-readingtimer;
				double processingtimer = omp_get_wtime();
				#endif
				//merge thread counters and compute the number of features
				for(int i=1; i<NPROCESSORS; ++i)
					th_usedfid[0] |= th_usedfid[i],
					th_ndps[0] += th_ndps[i];

				//make an array with trie data
				dplist **rlarray = rltrie.get_leaves();
				//make an array of used features ids
				unsigned int nfeatureids = th_usedfid[0].get_upcounter();
				unsigned int *usedfid = th_usedfid[0].get_uparray(nfeatureids);
				//set counters
				ndps = th_ndps[0],
				nrankedlists = rltrie.get_nleaves(),
				nfeatures = usedfid[nfeatureids-1]+1;
				//allocate memory
				#ifndef SKIP_DPDESCRIPTIONS
				descriptions = (char**)malloc(sizeof(char*)*ndps),
				#endif
				rloffsets = (unsigned int*)malloc(sizeof(unsigned int)*(nrankedlists+1)),
				rlids = (char**)malloc(sizeof(char*)*nrankedlists),
				labels = (float*)malloc(sizeof(float)*ndps);
				//compute 'rloffsets' values, i.e. prefixsum dplist sizes
				for(unsigned int i=0, sum=0; i<nrankedlists; ++i) {
					unsigned int rlsize = rlarray[i]->get_size();
					rloffsets[i] = sum;
					maxrlsize = rlsize>maxrlsize ? rlsize : maxrlsize,
					sum += rlsize;
				}
				rloffsets[nrankedlists] = ndps;
				//populate matrix (dp-major order)
				float **tmpfeatures = (float**)malloc(sizeof(float*)*ndps);
				#pragma omp parallel for
				for(unsigned int i=0; i<nrankedlists; ++i) {
					rlids[i] = rlarray[i]->get_rid();
					#ifdef PRESERVE_DPFILEORDER
					rlarray[i]->sort_bynline();
					#endif
					for(unsigned int j=rloffsets[i]; j<rloffsets[i+1]; ++j) {
						dp *current = rlarray[i]->front();
						#ifndef SKIP_DPDESCRIPTIONS
						descriptions[j] = current->get_description(),
						#endif
						tmpfeatures[j] = current->get_resizedfeatures(nfeatures),
						labels[j] = current->get_label();
						rlarray[i]->pop();
					}
				}
				//traspose feature matrix to get a feature-major order matrix
				features = (float**)malloc(sizeof(float*)*nfeatures);
				for(unsigned int i=0; i<nfeatures; ++i)
					features[i] = (float*)malloc(sizeof(float)*ndps);
				transpose(features, tmpfeatures, ndps, nfeatures);
				for(unsigned int i=0; i<ndps; ++i)
					free(tmpfeatures[i]);
				free(tmpfeatures);

				//delete feature arrays related to skipped featureids and compact the feature matrix
				for(unsigned int i=0, j=0; i<nfeatureids; ++i, ++j) {
					while(j!=usedfid[i])
						free(features[j++]);
					features[i] = features[j];
				}
				nfeatures = nfeatureids,
				features = (float**)realloc(features, sizeof(float*)*nfeatureids);
				//show statistics
				printf("\tfile = '%s'\n\tno. of datapoints = %u\n\tno. of ranked lists = %u\n\tmax ranked list size = %u\n\tno. of features = %u\n", filename, ndps, nrankedlists, maxrlsize, nfeatures);
				#ifdef SHOWTIMER
				processingtimer = omp_get_wtime()-processingtimer;
				printf("\telapsed time = reading: %.3f seconds (%.2f MB/s) + processing: %.3f seconds\n", readingtimer, filesize(filename)/readingtimer, processingtimer);
				#endif
				//free mem from temporary data structures
				delete[] usedfid,
				delete[] rlarray;
			} else exit(5);
		}
		~dpset() {
			if(features) for(unsigned int i=0; i<nfeatures; ++i) free(features[i]);
			if(rlids) for(unsigned int i=0; i<nrankedlists; ++i) free(rlids[i]);
			#ifndef SKIP_DPDESCRIPTIONS
			if(descriptions) for(unsigned int i=0; i<ndps; ++i) free(descriptions[i]);
			free(descriptions),
			#endif
			free(rloffsets),
			free(labels),
			free(features),
			free(rlids);
		}
		unsigned int get_nfeatures() const {
			return nfeatures;
		}
		unsigned int get_ndatapoints() const {
			return ndps;
		}
		unsigned int get_nrankedlists() const {
			return nrankedlists;
		}
		rnklst get_ranklist(unsigned int i) {
			return rnklst(rloffsets[i+1]-rloffsets[i], labels+rloffsets[i], rlids[i]);
		}
		float *get_fvector(unsigned int i) const {
			return features[i];
		}
		float **get_fmatrix() const {
			return features;
		}
		unsigned int *get_rloffsets() const {
			return rloffsets;
		}
		void sort_dpbyfeature(unsigned int i, unsigned int *&sorted, unsigned int &sortedsize) {
			sortedsize = ndps;
			sorted = idxnanfloat_radixsort(features[i], sortedsize);
		}
		float get_label(unsigned int i) const {
			return labels[i];
		}
	private:
		unsigned int nrankedlists = 0, ndps = 0, nfeatures = 0, maxrlsize = 0;
		unsigned int *rloffsets = NULL; //[0..nrankedlists] i-th rankedlist begins at rloffsets[i] and ends at rloffsets[i+1]-1
		float *labels = NULL; //[0..ndps-1]
		float **features = NULL; //[0..maxfid][0..ndps-1]
		char **rlids = NULL; //[0..nrankedlists-1]
		#ifndef SKIP_DPDESCRIPTIONS
		char **descriptions = NULL; //[0..ndps-1]
		#endif
};

#endif