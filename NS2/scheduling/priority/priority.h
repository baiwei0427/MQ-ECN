/*
 * Strict Priority Queueing (SP)
 *
 * Variables:
 * queue_num_: number of Class of Service (CoS) queues
 * thresh_: ECN marking threshold
 * mean_pktsize_: configured mean packet size in bytes
 * marking_scheme_: Per-queue ECN (0), Per-port ECN (1) and Latency ECN marking (4)
 */

#ifndef ns_priority_h
#define ns_priority_h

/*Maximum queue number */
#define MAX_QUEUE_NUM 64

/* Per-queue ECN marking */
#define PER_QUEUE_MARKING 0
/* Per-port ECN marking */
#define PER_PORT_MARKING 1
/* Dequeue latency-based ECN marking */
#define LATENCY_MARKING 4
/* PIE-like ECN marking */
#define PIE_MARKING 5

#define DQ_COUNT_INVALID -1

#include <string.h>
#include "queue.h"
#include "config.h"

class Priority : public Queue
{
	public:
		Priority();
		~Priority();
		virtual int command(int argc, const char*const* argv);

	protected:
		void enque(Packet*);	//enqueue function
		Packet* deque();	//dequeue function

		int MarkingECN(int q); // Determine whether we need to mark ECN, q is current queue number
		int TotalByteLength();	//return total queue length (bytes) of all the queues

		PacketQueue *queues;	//underlying multi-FIFO (CoS) queues
		double* dq_tstamps;	//measurement start time
		int* dq_counts;	//measured in bytes
		double* avg_dq_rates;	//average drain rate (bps)

		int mean_pktsize_;	//configured mean packet size in bytes
		int thresh_;	//ECN marking threshold
		int queue_num_;	//number of CoS queues. No more than MAX_QUEUE_NUM
		int marking_scheme_;	//ECN marking policy
		int dq_thresh_;	//threshold for departure rate estimation
		double estimate_rate_alpha_;	//factor between 0 and 1 for departure rate estimation
		double link_capacity_;	//link capacity (bps)
		int debug_;	//debug more(true) or not(false)

		Tcl_Channel total_qlen_tchan_;	//place to write total_qlen records
		Tcl_Channel qlen_tchan_;	//place to write per-queue qlen records
		void trace_total_qlen();	//routine to write total qlen records
		void trace_qlen();	//routine to write per-queue qlen records
};

#endif
