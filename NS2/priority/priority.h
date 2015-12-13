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

        PacketQueue *queues;	//underlying multi-FIFO (CoS) queues
		int mean_pktsize_;	//configured mean packet size in bytes
		int thresh_;	//ECN marking threshold
		int queue_num_;	//number of CoS queues. No more than MAX_QUEUE_NUM
		int marking_scheme_;	//Per-queue ECN (0), Per-port ECN (1) and Latency ECN (4)
		double link_capacity_;	//link capacity (bps)
		int debug_;	//debug more(true) or not(false)

		int TotalByteLength();	//return total queue length (bytes) of all the queues

		Tcl_Channel total_qlen_tchan_;	//place to write total_qlen records
		Tcl_Channel qlen_tchan_;	//place to write per-queue qlen records
		void trace_total_qlen();	//routine to write total qlen records
		void trace_qlen();	//routine to write per-queue qlen records
};

#endif
