/* ------------------------------------------------
Copyright 2014 AT&T Intellectual Property
   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 ------------------------------------------- */

#ifndef MERGE_OPERATOR_H
#define MERGE_OPERATOR_H

#include "host_tuple.h"
#include "base_operator.h"
#include <list>
using namespace std;

#include <stdio.h>


//int last_tb = 0;

template <class merge_functor>
class merge_operator : public base_operator {
private :
	// input channel on which we want to receive the next tuple
	// value -1 indicates that we never received a single tuple
	// and we are not concerned on which channel tuple will arrive
	int wait_channel;

	// list of tuples from one of the channel waiting to be compared
	// against tuple from the other channel
	list<host_tuple> merge_queue;
	int merge_qsize;

	// comparator object used to compare the tuples
	merge_functor func;

	// soft limit on queue size - we consider operator blocked on its input
	// whenever we reach this soft limit (not used anymore)
	size_t soft_queue_size_limit;

	int dropped_cnt;

	// memory footprint for the merge queue in bytes
	unsigned int queue_mem;

	// appends tuple to the end of the merge_queue
	// if tuple is stack resident, makes it heap resident
	void append_tuple(host_tuple& tup) {
		if (!tup.heap_resident) {
			char* data = (char*)malloc(tup.tuple_size);
			memcpy(data, tup.data, tup.tuple_size);
			tup.data = data;
			tup.heap_resident = true;
		}
		merge_queue.push_back(tup);
		merge_qsize++;
		queue_mem += tup.tuple_size;
	}

	void purge_queue(int channel, list<host_tuple>& result){
		if (merge_queue.empty())
			return;
		host_tuple top_tuple = merge_queue.front();
		// remove all the tuple smaller than arrived tuple
		while(func.compare_stored_with_temp_status(top_tuple, 1-channel) <= 0) {
			merge_queue.pop_front();
			merge_qsize--;
			queue_mem -= top_tuple.tuple_size;
			if(merge_qsize<0) abort();
			func.update_stored_temp_status(top_tuple,channel);
			func.xform_tuple(top_tuple);
			top_tuple.channel = output_channel;
			result.push_back(top_tuple);

			if (merge_queue.empty())
				break;

			top_tuple = merge_queue.front();
			func.update_stored_temp_status(top_tuple,channel);
		}
	}



public:

	merge_operator(int schema_handle1, int schema_handle2, size_t size_limit, const char* name ) : base_operator(name), func(schema_handle1) {
		wait_channel = -1;
		soft_queue_size_limit = size_limit;
		merge_qsize = 0;
		dropped_cnt = 0;
		queue_mem = 0;
	}


	int accept_tuple(host_tuple& tup, list<host_tuple>& result) {
		int last_channel;

		if (tup.channel > 1) {
			fprintf(stderr, "Illegal channel number %d for two-way merge\n", tup.channel);
			return 0;
		}

		/* reject "out of order" tuple - we can receive those after forced flush */
		func.get_timestamp(tup);
		int res = func.compare_with_temp_status(tup.channel);
		bool is_temp_tuple = func.temp_status_received(tup);

/*
//			Ignore temp tuples until we can fix their timestamps.
if(is_temp_tuple){
 tup.free_tuple();
 return 0;
}*/
		if (res < 0){	// out of order tuple

			if (++dropped_cnt % 100000 == 0) {
				gslog(LOG_ALERT, "%d tuples dropped by %s  merge\n", dropped_cnt,get_name());
			}
			//if(func.print_warnings())
			//	fprintf(stderr,"Warning: merge %s receives an out-of-order tuple on channel %d.\n", get_name(), tup.channel);
			// free tuple memory
			tup.free_tuple();
			return 0;
		} else {

			if (wait_channel < 0 || wait_channel != tup.channel) {
				if (wait_channel < 0)
					func.update_temp_status(tup);

				if (!is_temp_tuple){
					if(func.compare_with_temp_status(1-tup.channel) <= 0){
						if (!tup.heap_resident) {
							char* data = (char*)malloc(tup.tuple_size);
							memcpy(data, tup.data, tup.tuple_size);
							tup.data = data;
							tup.heap_resident = true;
						}
						func.xform_tuple(tup);
						tup.channel = output_channel;
						result.push_back(tup);
					}else{
						append_tuple(tup);		// put arrived tuple in the queue
					}
				}
//				func.update_temp_status_by_slack(tup, 1-tup.channel);
//				purge_queue(tup.channel, result);

				wait_channel = 1-tup.channel;
			}else{
	//				If possible, clear tuples from the other queue.
				func.update_temp_status(tup);
				purge_queue(1-tup.channel, result);

				if(func.compare_with_temp_status(1-tup.channel) <= 0) {	// other tuples in the queue are larger that arrived tuple
					if (!is_temp_tuple) {
						if (!tup.heap_resident) {
                                                        char* data = (char*)malloc(tup.tuple_size);
                                                        memcpy(data, tup.data, tup.tuple_size);
                                                        tup.data = data;
                                                        tup.heap_resident = true;
                                                }
						func.xform_tuple(tup);
						tup.channel = output_channel;
						result.push_back(tup);
					}
				}
				else {
					if (!is_temp_tuple)
						append_tuple(tup);		// put arrived tuple in the queue
					wait_channel = 1 - tup.channel;	// now we want the tuple from other channel
				}
			}

		}

		// temp status tuples emited by merge don't serve any other purpose
		// other than tracing the flow of tuples
		if (is_temp_tuple) {
			host_tuple temp_tup;
			if (!func.create_temp_status_tuple(temp_tup)) {
				temp_tup.channel = output_channel;
				result.push_back(temp_tup);
			}
			// clear memory of heap-resident temporal tuples
			tup.free_tuple();
		}

		if (!merge_qsize)
			wait_channel = -1;

		return 0;
	}

	int flush(list<host_tuple>& result) {

		if (merge_queue.empty())
			return 0;

		host_tuple top_tuple;
		list<host_tuple>::iterator iter;
		for (iter = merge_queue.begin(); iter != merge_queue.end(); iter++) {
			top_tuple = *iter;
			func.update_stored_temp_status(top_tuple,top_tuple.channel);
			func.xform_tuple(top_tuple);
			top_tuple.channel = output_channel;
			result.push_back(top_tuple);
		}

		queue_mem = 0;

		return 0;
	}

	int set_param_block(int sz, void * value) {
			return 0;
	}


	int get_temp_status(host_tuple& result) {
		result.channel = output_channel;
		return func.create_temp_status_tuple(result);
	}


	int get_blocked_status () {
		if (merge_qsize> soft_queue_size_limit)
			return wait_channel;
		else
			return -1;
	}

	unsigned int get_mem_footprint() {
		return queue_mem;
	}

};

#endif	// MERGE_OPERATOR_H
