#ifndef ns_wrr_h
#define ns_wrr_h

#include "queue.h"
#include "config.h"
#include "trace.h"

/*Maximum queue number */
#define MAX_QUEUE_NUM 64

/* Per-queue ECN marking */
#define PER_QUEUE_MARKING 0
/* Per-port ECN marking */
#define PER_PORT_MARKING 1
/* MQ-ECN for any packet scheduling algorithms */
//#define MQ_MARKING_GENER 2
/* MQ-ECN for round robin packet scheduling algorithms */
#define MQ_MARKING_RR 3
/* Dequeue latency-based ECN marking */
#define LATENCY_MARKING 4
/* PIE-like ECN marking */
#define PIE_MARKING 5

#define DQ_COUNT_INVALID -1

class PacketWRR;
class WRR;

class PacketWRR: public PacketQueue
{
	public:
		PacketWRR(): quantum(1500), counter(0), thresh(0), start_time(0), dq_tstamp(0), dq_count(DQ_COUNT_INVALID), avg_dq_rate(-1), counter_updated(false) {}

		int quantum;	//quantum of this queue
		int counter;	//counter for bytes that can be sent in this round
		double thresh;	// per-queue ECN marking threshold (pkts)
		double start_time;	//time when the queue waits for scheduling in this round
		double dq_tstamp;	//measurement start time
		int dq_count;	//measured in bytes
		double avg_dq_rate;	//average drain rate (bps)
		bool counter_updated; //whether the counter has been updated in this round

		friend class WRR;
};

class WRR : public Queue
{
	public:
		WRR();
		~WRR();
		virtual int command(int argc, const char*const* argv);

	protected:
		Packet *deque(void);
		void enque(Packet *pkt);
		int TotalByteLength();	//Get total length of all queues in bytes
		int MarkingECN(int q);	//Determine whether we need to mark ECN, q is current queue number

		/* Variables */
		PacketWRR *queues;	//underlying multi-FIFO (CoS) queues
		double round_time;	//Round time estimation value
		double last_idle_time;	//Last time when link becomes idle
		bool init;
		int current;	//current queue ID

		int queue_num_;	//number of queues
		int mean_pktsize_;	//MTU in bytes
		double port_thresh_;	//per-port ECN marking threshold (pkts)
		int marking_scheme_;	//ECN marking policy
		double estimate_round_alpha_;	//factor between 0 and 1 for round time estimation
		int estimate_round_idle_interval_bytes_;	//Time interval (divided by link capacity) to update round time when link is idle.
		double link_capacity_;	//Link capacity
		int dq_thresh_;	//threshold for departure rate estimation
		double estimate_rate_alpha_;	//factor between 0 and 1 for departure rate estimation
		int debug_;	//debug more(true) or not(false)

		Tcl_Channel total_qlen_tchan_;	//place to write total_qlen records
		Tcl_Channel qlen_tchan_;	//place to write per-queue qlen records
		void trace_total_qlen();	//routine to write total qlen records
		void trace_qlen();	//routine to write per-queue qlen records
};

#endif
