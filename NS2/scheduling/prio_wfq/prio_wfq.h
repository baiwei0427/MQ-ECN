#ifndef ns_prio_wfq_h
#define ns_prio_wfq_h

#include "queue.h"
#include "config.h"
#include "trace.h"
#include "timer-handler.h"

#include <iostream>
#include <queue>
using namespace std;

/* Maximum number of strict higher priority queues */
#define MAX_PRIO_QUEUE_NUM 8
/* Maximum number of WFQ queues in the lowest priority */
#define MAX_WFQ_QUEUE_NUM 64

/* Per-queue ECN marking */
#define PER_QUEUE_MARKING 0
/* Per-port ECN marking */
#define PER_PORT_MARKING 1
/* MQ-ECN for any packet scheduling algorithms */
#define MQ_MARKING_GENER 2
/* Dequeue latency-based ECN marking */
#define LATENCY_MARKING 4
/* PIE-like ECN marking */
#define PIE_MARKING 5

#define DQ_COUNT_INVALID -1

/* Types of queues */
#define PRIO_QUEUE 0
#define WFQ_QUEUE 1

class PacketPRIO;   //strict higher priority queues
class PacketWFQ;    //WFQ queues in the lowest priority
class PRIO_WFQ;

class PRIO_WFQ_Timer : public TimerHandler
{
public:
	PRIO_WFQ_Timer(PRIO_WFQ *q) : TimerHandler() { queue_ = q;}

protected:
	virtual void expire(Event *e);
	PRIO_WFQ *queue_;
};

class PacketPRIO: public PacketQueue
{
	public:
		PacketPRIO(): thresh(0), dq_tstamp(0), dq_count(DQ_COUNT_INVALID), avg_dq_rate(-1) {}

		double thresh;	//per-queue ECN marking threshold (pkts)
		double dq_tstamp;	//measurement start time
		int dq_count;	//measured in bytes
		double avg_dq_rate;	//average drain rate (bps)

		friend class PRIO_WFQ;
};

class PacketWFQ : public PacketQueue
{
	public:
		PacketWFQ(): weight(10000.0), headFinishTime(0), thresh(0), dq_tstamp(0), dq_count(DQ_COUNT_INVALID), avg_dq_rate(-1) {}

		double weight;    //weight of the service
  		long double headFinishTime; //finish time of the packet at head of this queue.
		double thresh;    //per-queue ECN marking threshold (pkts)
		double dq_tstamp; //measurement start time
		int dq_count; //measured in bytes
		double avg_dq_rate;   //average drain rate (bps)

		friend class PRIO_WFQ;
};

class PRIO_WFQ : public Queue
{
	public:
		PRIO_WFQ();
		~PRIO_WFQ();
		void timeout(int);
		virtual int command(int argc, const char*const* argv);

	protected:
		Packet* deque(void);
		void enque(Packet *pkt);
        int TotalByteLength();  //Get total length of all queues in bytes
		int Total_WFQ_ByteLength();   //Get total length of WFQ queues in bytes
		int Total_Prio_ByteLength();  //Get total length of higher priority queues in bytes
		int MarkingECN(int q);    //Determine whether we need to mark ECN, q is current queue number

		/* Variables */
        PacketPRIO *prio_queues;	//strict higher priority queues
        PacketWFQ *wfq_queues;   //WFQ queues in the lowest priority

        long double currTime; //Finish time assigned to last packet
        PRIO_WFQ_Timer timer_;  //timer for weight_sum_estimate update
        double weight_sum_estimate;	//estimation value for sum of weights of all non-empty  queues
        double weight_sum;	//sum of weights of all non-empty queues
        double last_update_time;	//last time when we update quantum_sum_estimate
        double last_idle_time;	//Last time when link becomes idle
        int init;	//whether the timer has been started

        int prio_queue_num_;    //number of higher priority queues
        int wfq_queue_num_; //number of WFQ queues
        int mean_pktsize_;    //MTU in bytes
        double port_thresh_;  //per-port ECN marking threshold (pkts)
        int marking_scheme_;  //ECN marking policy
        double estimate_weight_alpha_;    //factor between 0 and 1 for weight estimation
        double estimate_weight_interval_bytes_;   //time interval is estimate_weight_interval_bytes_/link capacity.
        int estimate_weight_enable_timer_;    //whether we use real timer (TimerHandler) for weight estimation
        int dq_thresh_;   //threshold for departure rate estimation
        double estimate_rate_alpha_;  //factor between 0 and 1 for departure rate estimation
        double link_capacity_;    //Link capacity
        int debug_;   //debug more(true) or not(false)

        Tcl_Channel total_qlen_tchan_;  //Place to write total_qlen records
        Tcl_Channel qlen_tchan_;    //Place to write per-queue qlen records
        void trace_total_qlen();    //Routine to write total qlen records
        void trace_qlen();  //Routine to write per-queue qlen records
};

#endif
