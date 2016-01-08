#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include "flags.h"
#include "prio_dwrr.h"

#define max(arg1,arg2) (arg1>arg2 ? arg1 : arg2)
#define min(arg1,arg2) (arg1<arg2 ? arg1 : arg2)

/* Insert a queue to the tail of an active list. Return true if insert succeeds */
static void InsertTailList(PacketDWRR* list, PacketDWRR *q)
{
	if (list && q)
	{
		PacketDWRR* tmp = list;
		while (true)
		{
			/* Arrive at the tail of this list */
			if (!(tmp->next))
			{
				tmp->next = q;
				q->next = NULL;
				return;
			}
			/* Move to next node */
			else
				tmp = tmp->next;
		}
	}
}

/* Remove and return the head node from the active list */
static PacketDWRR* RemoveHeadList(PacketDWRR* list)
{
	if (list)
	{
		PacketDWRR* tmp = list->next;
		if (tmp)
		{
			list->next = tmp->next;
			return tmp;
		}
		/* This list is empty */
		else
			return NULL;
	}
	else
		return NULL;
}

static class PrioDwrrClass : public TclClass
{
	public:
		PrioDwrrClass() : TclClass("Queue/PrioDwrr") {}
		TclObject* create(int argc, const char*const* argv)
		{
			return (new PRIO_DWRR);
		}
} class_prio_dwrr;

PRIO_DWRR::PRIO_DWRR(): timer_(this)
{
	prio_queues = new PacketPRIO[MAX_PRIO_QUEUE_NUM];
	dwrr_queues = new PacketDWRR[MAX_DWRR_QUEUE_NUM];

	for (int i = 0; i < MAX_PRIO_QUEUE_NUM; i++)
		prio_queues[i].id = i;
	for (int i = 0; i < MAX_DWRR_QUEUE_NUM; i++)
		dwrr_queues[i].id = i;

	activeList = new PacketDWRR();
	round_time = 0;
	quantum_sum = 0;
	quantum_sum_estimate = 0;
	init = 0;
	last_update_time = 0;
	last_idle_time = 0;

	total_qlen_tchan_ = NULL;
	qlen_tchan_ = NULL;

	prio_queue_num_ = 1;
	dwrr_queue_num_ = 7;
	mean_pktsize_ = 1500;
	port_thresh_ = 65;
	marking_scheme_ = 0;
	estimate_round_alpha_ = 0.75;
	estimate_round_idle_interval_bytes_ = 1500;
	estimate_quantum_alpha_ = 0.75;
	estimate_quantum_interval_bytes_ = 1500;
	estimate_quantum_enable_timer_ = 0;
	dq_thresh_ = 10000;
	estimate_rate_alpha_ = 0.875;
	link_capacity_ = 10000000000;	//10Gbps
	debug_ = 0;

	/* bind variables */
	bind("prio_queue_num_", &prio_queue_num_);
	bind("dwrr_queue_num_", &dwrr_queue_num_);
	bind("mean_pktsize_", &mean_pktsize_);
	bind("port_thresh_", &port_thresh_);
	bind("marking_scheme_", &marking_scheme_);
	bind("estimate_round_alpha_", &estimate_round_alpha_);
	bind("estimate_round_idle_interval_bytes_", &estimate_round_idle_interval_bytes_);
	bind("estimate_quantum_alpha_", &estimate_quantum_alpha_);
	bind("estimate_quantum_interval_bytes_", &estimate_quantum_interval_bytes_);
	bind_bool("estimate_quantum_enable_timer_", &estimate_quantum_enable_timer_);
	bind("dq_thresh_", &dq_thresh_);
	bind("estimate_rate_alpha_", &estimate_rate_alpha_);
	bind_bw("link_capacity_", &link_capacity_);
	bind_bool("debug_", &debug_);
}

PRIO_DWRR::~PRIO_DWRR()
{
	delete activeList;
	delete [] prio_queues;
	delete [] dwrr_queues;
	timer_.cancel();
}

void PRIO_DWRR::timeout(int)
{
	/* update quantum_sum_estimate */
	quantum_sum_estimate = quantum_sum_estimate * estimate_quantum_alpha_ + quantum_sum * (1 - estimate_quantum_alpha_);

	if (debug_ && marking_scheme_ == MQ_MARKING_GENER)
		printf("%.9f smooth quantum sum: %f, sample quantum sum: %d\n", Scheduler::instance().clock(), quantum_sum_estimate, quantum_sum);

	/* reschedule timer */
	if (link_capacity_ > 0)
		timer_.resched(estimate_quantum_interval_bytes_ * 8 / link_capacity_);
}

void PRIO_DWRR_Timer::expire(Event* e)
{
	queue_->timeout(0);
}

/* Get total length of all DWRR queues in bytes */
int PRIO_DWRR::Total_DWRR_ByteLength()
{
	int result = 0;

	for (int i = 0; i < MAX_DWRR_QUEUE_NUM; i++)
		result += dwrr_queues[i].byteLength();

	return result;
}

/* Get total length of all higher priority queues in bytes */
int PRIO_DWRR::Total_Prio_ByteLength()
{
	int result = 0;

	for (int i = 0; i < MAX_PRIO_QUEUE_NUM; i++)
		result += prio_queues[i].byteLength();

	return result;
}

/* Get total length of all queues in bytes */
int PRIO_DWRR::TotalByteLength()
{
	return Total_DWRR_ByteLength() + Total_Prio_ByteLength();
}


/* Determine whether we need to mark ECN.
 * Return 1 if it requires marking
 */
int PRIO_DWRR::MarkingECN(int queue_index)
{
	int type = 0;
	int prio_queue_index = 0;
	int dwrr_queue_index = 0;

	if (queue_index < 0 || queue_index >= prio_queue_num_ + dwrr_queue_num_)
	{
		fprintf(stderr, "illegal queue index value %d\n", queue_index);
		exit(1);
	}

	if (queue_index < prio_queue_num_)
	{
		prio_queue_index = queue_index;
		type = PRIO_QUEUE;
	}
	else
	{
		dwrr_queue_index = queue_index - prio_queue_num_;
		type = DWRR_QUEUE;
	}

	/* Per-queue ECN marking */
	if (marking_scheme_ == PER_QUEUE_MARKING)
	{
		if (type == PRIO_QUEUE && prio_queues[prio_queue_index].byteLength() > prio_queues[prio_queue_index].thresh * mean_pktsize_)
			return 1;
		else if (type == DWRR_QUEUE && dwrr_queues[dwrr_queue_index].byteLength() > dwrr_queues[dwrr_queue_index].thresh * mean_pktsize_)
			return 1;
		else
			return 0;
	}
	/* Per-port ECN marking */
	else if (marking_scheme_ == PER_PORT_MARKING)
	{
		if (TotalByteLength() > port_thresh_ * mean_pktsize_)
			return 1;
		else
			return 0;
	}
	/* MQ-ECN for any packet scheduling algorithms */
	else if (marking_scheme_ == MQ_MARKING_GENER)
	{
		if (type == DWRR_QUEUE)
		{
			double thresh = 0;
			if (quantum_sum_estimate >= 0.000000001)
				thresh = min(dwrr_queues[dwrr_queue_index].quantum / quantum_sum_estimate, 1) * port_thresh_;
			else
				thresh = port_thresh_;

			if (dwrr_queues[dwrr_queue_index].byteLength() > thresh * mean_pktsize_)
				return 1;
			else
				return 0;
		}
		else if (prio_queues[prio_queue_index].byteLength() > prio_queues[prio_queue_index].thresh * mean_pktsize_)
			return 1;
		else
			return 0;
	}
	/* MQ-ECN for round robin packet scheduling algorithms */
	else if (marking_scheme_ == MQ_MARKING_RR)
	{
		if (type == DWRR_QUEUE)
		{
			double thresh = 0;
			if (round_time >= 0.000000001 && link_capacity_ > 0)
				thresh = min(dwrr_queues[dwrr_queue_index].quantum * 8 / round_time / link_capacity_, 1) * port_thresh_;
			else
				thresh = port_thresh_;
			//For debug
			//printf("round time: %f threshold: %f\n",round_time, thresh);
			if (dwrr_queues[dwrr_queue_index].byteLength() > thresh * mean_pktsize_)
				return 1;
			else
				return 0;
		}
		else if (prio_queues[prio_queue_index].byteLength() > prio_queues[prio_queue_index].thresh * mean_pktsize_)
			return 1;
		else
			return 0;
	}
	/* PIE-like ECN marking */
	else if (marking_scheme_ == PIE_MARKING)
	{
		double thresh = 0;

		if (type == DWRR_QUEUE && dwrr_queues[dwrr_queue_index].avg_dq_rate >= 0.000000001 && link_capacity_ > 0)
			thresh = min(dwrr_queues[dwrr_queue_index].avg_dq_rate / link_capacity_, 1) * port_thresh_;
		else if (type == PRIO_QUEUE && prio_queues[prio_queue_index].avg_dq_rate >= 0.000000001 && link_capacity_ > 0)
			thresh = min(prio_queues[prio_queue_index].avg_dq_rate / link_capacity_, 1) * port_thresh_;
		else
			thresh = port_thresh_;

		if (type == DWRR_QUEUE && dwrr_queues[dwrr_queue_index].byteLength() > thresh * mean_pktsize_)
			return 1;
		else if (type == PRIO_QUEUE && prio_queues[prio_queue_index].byteLength() > thresh * mean_pktsize_)
			return 1;
		else
			return 0;
	}
	/* Unknown ECN marking scheme */
	else
	{
		fprintf (stderr,"Unknown ECN marking scheme\n");
		return 0;
	}
}

/*
 *  entry points from OTcL to set per queue state variables
 *   - $q set-quantum queue_id queue_quantum (quantum is actually weight)
 *   - $q set-thresh queue_id queue_thresh
 *   - $q attach-total file
 *	 - $q attach-queue file
 *
 *  NOTE: $q represents the discipline queue variable in OTcl.
 */
int PRIO_DWRR::command(int argc, const char*const* argv)
{
	if (argc == 3)
	{
		// attach a file to trace total queue length
		if (strcmp(argv[1], "attach-total") == 0)
		{
			int mode;
			const char* id = argv[2];
			Tcl& tcl = Tcl::instance();
			total_qlen_tchan_ = Tcl_GetChannel(tcl.interp(), (char*)id, &mode);
			if (total_qlen_tchan_ == 0)
			{
				tcl.resultf("PRIO_DWRR: trace: can't attach %s for writing", id);
				return (TCL_ERROR);
			}
			return (TCL_OK);
		}
		else if (strcmp(argv[1], "attach-queue") == 0)
		{
			int mode;
			const char* id = argv[2];
			Tcl& tcl = Tcl::instance();
			qlen_tchan_ = Tcl_GetChannel(tcl.interp(), (char*)id, &mode);
			if (qlen_tchan_ == 0)
			{
				tcl.resultf("PRIO_DWRR: trace: can't attach %s for writing", id);
				return (TCL_ERROR);
			}
			return (TCL_OK);
		}
	}
	else if (argc == 4)
	{
		if (strcmp(argv[1], "set-quantum")==0)
		{
			int dwrr_queue_index = atoi(argv[2]) - prio_queue_num_;
			if (dwrr_queue_index < dwrr_queue_num_ && dwrr_queue_index >= 0)
			{
				int quantum = atoi(argv[3]);
				if (quantum > 0)
				{
					dwrr_queues[dwrr_queue_index].quantum = quantum;
					return (TCL_OK);
				}
				else
				{
					fprintf(stderr, "illegal quantum value %d for DWRR queue %d\n", quantum, dwrr_queue_index);
					exit(1);
				}
			}
			/* Exceed the maximum queue number or smaller than 0*/
			else
			{
				fprintf(stderr, "illegal DWRR queue index value %d\n", dwrr_queue_index);
				exit(1);
			}
		}
		else if (strcmp(argv[1], "set-thresh") == 0)
		{
			int queue_index = atoi(argv[2]);
			if (queue_index < prio_queue_num_ + dwrr_queue_num_ && queue_index >= 0)
			{
				double thresh = atof(argv[3]);
				if (thresh >= 0)
				{
					if (queue_index < prio_queue_num_)
						prio_queues[queue_index].thresh = thresh;
					else
						dwrr_queues[queue_index - prio_queue_num_].thresh = thresh;
					return (TCL_OK);
				}
				else
				{
					fprintf(stderr, "illegal thresh value %s for queue %s\n", argv[3], argv[2]);
					exit(1);
				}
			}
			/* Exceed the maximum queue number or smaller than 0*/
			else
			{
				fprintf(stderr, "illegal queue index value %s\n", argv[2]);
				exit(1);
			}
		}
	}
	return (Queue::command(argc, argv));
}

/* Receive a new packet */
void PRIO_DWRR::enque(Packet *p)
{
	hdr_ip *iph = hdr_ip::access(p);
	int prio = iph->prio();
	hdr_flags* hf = hdr_flags::access(p);
	hdr_cmn* hc = hdr_cmn::access(p);
	int pktSize = hc->size();
	int qlimBytes = qlim_*mean_pktsize_;
	dwrr_queue_num_ = max(min(dwrr_queue_num_, MAX_DWRR_QUEUE_NUM), 1);
	prio_queue_num_ = max(min(prio_queue_num_, MAX_PRIO_QUEUE_NUM), 1);
	int queue_num_ = dwrr_queue_num_ + prio_queue_num_;

	if (init == 0)
	{
		/* Start timer*/
		if (marking_scheme_ == MQ_MARKING_GENER && estimate_quantum_enable_timer_ && estimate_quantum_interval_bytes_ > 0 && link_capacity_ > 0)
			timer_.resched(estimate_quantum_interval_bytes_  *8 / link_capacity_);
		init = 1;
	}

	if (Total_DWRR_ByteLength() == 0 && marking_scheme_ == MQ_MARKING_GENER && !estimate_quantum_enable_timer_)
	{
		double now = Scheduler::instance().clock();
		double idleTime = now-last_idle_time;

		if (estimate_quantum_interval_bytes_ > 0 && link_capacity_ > 0)
			quantum_sum_estimate = quantum_sum_estimate * pow(estimate_quantum_alpha_, idleTime / (estimate_quantum_interval_bytes_ * 8 / link_capacity_));
		else
			quantum_sum_estimate = 0;
		last_update_time = now;

		if (debug_)
			printf("%.9f smooth quantum sum is reset to %f\n", now, quantum_sum_estimate);
	}
	else if (Total_DWRR_ByteLength() == 0 && marking_scheme_ == MQ_MARKING_RR)
	{
		double now = Scheduler::instance().clock();
		double idleTime = now-last_idle_time;
		int intervalNum = 0;
		if (estimate_round_idle_interval_bytes_ > 0 && link_capacity_ > 0)
		{
			intervalNum = (int)(idleTime / (estimate_round_idle_interval_bytes_ * 8 / link_capacity_));
			round_time = round_time * pow(estimate_quantum_alpha_,intervalNum);
		}
		else
		{
			round_time = 0;
		}

		last_update_time = now;
		if(debug_)
			printf("%.9f smooth round time is reset to %f after %d idle time slots\n", now, round_time, intervalNum);
	}

	/* The shared buffer is overfilld */
	if (TotalByteLength() + pktSize > qlimBytes)
	{
		drop(p);
		//printf("Packet drop\n");
		return;
	}

	if (prio >= queue_num_ || prio < 0)
		prio = queue_num_ - 1;

	if (prio < prio_queue_num_)
		prio_queues[prio].enque(p);
	else
	{
		int dwrr_queue_index = prio - prio_queue_num_;
		dwrr_queues[dwrr_queue_index].enque(p);
		/* if queues[dwrr_queue_index] is not in activeList */
		if (dwrr_queues[dwrr_queue_index].active == false)
		{
			dwrr_queues[dwrr_queue_index].deficitCounter = 0;
			dwrr_queues[dwrr_queue_index].active = true;
			dwrr_queues[dwrr_queue_index].current = false;
			dwrr_queues[dwrr_queue_index].start_time = Scheduler::instance().clock();	//Start time of this round
			InsertTailList(activeList, &dwrr_queues[dwrr_queue_index]);
			quantum_sum += dwrr_queues[dwrr_queue_index].quantum;
		}
	}

	/* Enqueue ECN marking */
	if (marking_scheme_ != LATENCY_MARKING && MarkingECN(prio) > 0 && hf->ect())
		hf->ce() = 1;
	/* For dequeue latency ECN marking ,record enqueue timestamp here */
	else if (marking_scheme_ == LATENCY_MARKING && hf->ect())
		hc->timestamp() = Scheduler::instance().clock();

	trace_qlen();
	trace_total_qlen();
}

Packet *PRIO_DWRR::deque(void)
{
	PacketDWRR *headNode = NULL;
	Packet *pkt = NULL;
	hdr_flags* hf = NULL;
	hdr_cmn* hc = NULL;
	int pktSize = 0;
	double round_time_sample = 0;
	double sojourn_time = 0;
	double latency_thresh = 0;

	if (Total_Prio_ByteLength() > 0)
	{
		for (int i = 0; i < prio_queue_num_; i++)
		{
			if (prio_queues[i].byteLength() > 0)
			{
				pkt = prio_queues[i].deque();
				pktSize = hdr_cmn::access(pkt)->size();

				/* dequeue latency-based ECN marking */
				if (marking_scheme_ == LATENCY_MARKING)
				{
					hc = hdr_cmn::access(pkt);
					hf = hdr_flags::access(pkt);
					sojourn_time = Scheduler::instance().clock() - hc->timestamp();
					if (link_capacity_ > 0)
						latency_thresh = port_thresh_ * mean_pktsize_ * 8 / link_capacity_;

					if (hf->ect() && sojourn_time > latency_thresh)
					{
						hf->ce() = 1;
						if (debug_)
							printf("sojourn time %.9f > threshold %.9f\n", sojourn_time, latency_thresh);
					}
					hc->timestamp() = 0;
				}

				/* If current queue is about 10KB or more and dq_count is unset
				 * we have enough packets to calculate the drain rate. Save
				 * current time as dq_tstamp and start measurement cycle.
				 */
				if (prio_queues[i].byteLength() >= dq_thresh_ && prio_queues[i].dq_count == DQ_COUNT_INVALID)
				{
					prio_queues[i].dq_tstamp = Scheduler::instance().clock();
					prio_queues[i].dq_count = 0;
				}

				/* Calculate the average drain rate from this value.  If queue length
				 * has receded to a small value viz., <= dq_thresh_bytes,reset
				 * the dq_count to -1 as we don't have enough packets to calculate the
				 * drain rate anymore The following if block is entered only when we
				 * have a substantial queue built up (dq_thresh_ bytes or more)
				 * and we calculate the drain rate for the threshold here.*/
				if (prio_queues[i].dq_count != DQ_COUNT_INVALID)
				{
					prio_queues[i].dq_count += pktSize;
					if (prio_queues[i].dq_count >= dq_thresh_)
					{
						//take transmission time into account
						double interval = Scheduler::instance().clock() - prio_queues[i].dq_tstamp + pktSize * 8 / link_capacity_;
						double rate = prio_queues[i].dq_count * 8 / interval;

						if (prio_queues[i].avg_dq_rate < 0)
							prio_queues[i].avg_dq_rate = rate;
						else
							prio_queues[i].avg_dq_rate = prio_queues[i].avg_dq_rate * estimate_rate_alpha_ + rate * (1 - estimate_rate_alpha_);

						/* If the queue has receded below the threshold, we hold
						 * on to the last drain rate calculated, else we reset
						 * dq_count to 0 to re-enter the if block when the next
						 * packet is dequeued
						 */
						if (prio_queues[i].byteLength() < dq_thresh_)
							prio_queues[i].dq_count = DQ_COUNT_INVALID;
						else
						{
							prio_queues[i].dq_count = 0;
							//take transmission time into account
							prio_queues[i].dq_tstamp = Scheduler::instance().clock() + pktSize * 8 / link_capacity_;
						}

						if (debug_ && marking_scheme_ == PIE_MARKING)
							printf("[queue %d] sample departure rate : %.2f average departure rate: %.2f\n", prio_queues[i].id, rate, prio_queues[i].avg_dq_rate);
					}
				}
				break;
			}
		}

		return pkt;
	}
	else if (Total_DWRR_ByteLength() > 0)
	{
		/* We must go through all actives queues and select a packet to dequeue */
		while (1)
		{
			headNode = activeList->next;	//Get head node from activeList
			if(headNode == NULL)
				fprintf (stderr,"no active flow\n");

			/* if headNode is not empty */
			if (headNode->length() > 0)
			{
				/* headNode has not been served yet in this round */
				if (headNode->current == false)
				{
					headNode->deficitCounter += headNode->quantum;
					headNode->current = true;
				}

				pktSize = hdr_cmn::access(headNode->head())->size();
				/* if we have enough quantum to dequeue the head packet */
				if (pktSize <= headNode->deficitCounter)
				{
					pkt = headNode->deque();
					headNode->deficitCounter -= pktSize;

					/* dequeue latency-based ECN marking */
					if (marking_scheme_ == LATENCY_MARKING)
					{
						hc = hdr_cmn::access(pkt);
						hf = hdr_flags::access(pkt);
						sojourn_time = Scheduler::instance().clock() - hc->timestamp();
						if (link_capacity_ > 0)
							latency_thresh = port_thresh_ * mean_pktsize_ * 8 / link_capacity_;

						if (hf->ect() && sojourn_time > latency_thresh)
						{
							hf->ce() = 1;
							if (debug_)
								printf("sojourn time %.9f > threshold %.9f\n", sojourn_time, latency_thresh);
						}
						hc->timestamp() = 0;
					}

					/* If current queue is about 10KB or more and dq_count is unset
					 * we have enough packets to calculate the drain rate. Save
					 * current time as dq_tstamp and start measurement cycle.
					 */
					if (headNode->byteLength() >= dq_thresh_ && headNode->dq_count == DQ_COUNT_INVALID)
					{
						headNode->dq_tstamp = Scheduler::instance().clock();
						headNode->dq_count = 0;
					}

					/* Calculate the average drain rate from this value.  If queue length
					 * has receded to a small value viz., <= dq_thresh_bytes,reset
					 * the dq_count to -1 as we don't have enough packets to calculate the
					 * drain rate anymore The following if block is entered only when we
					 * have a substantial queue built up (dq_thresh_ bytes or more)
					 * and we calculate the drain rate for the threshold here.*/
					if (headNode->dq_count != DQ_COUNT_INVALID)
					{
						headNode->dq_count += pktSize;
						if (headNode->dq_count >= dq_thresh_)
						{
							//take transmission time into account
							double interval = Scheduler::instance().clock() - headNode->dq_tstamp + pktSize * 8 / link_capacity_;
							double rate = headNode->dq_count * 8 / interval;

							if (headNode->avg_dq_rate < 0)
								headNode->avg_dq_rate = rate;
							else
								headNode->avg_dq_rate = headNode->avg_dq_rate * estimate_rate_alpha_ + rate * (1 - estimate_rate_alpha_);

							/* If the queue has receded below the threshold, we hold
	 						 * on to the last drain rate calculated, else we reset
	 					 	 * dq_count to 0 to re-enter the if block when the next
	 					     * packet is dequeued
	 					 	 */
							if (headNode->byteLength() < dq_thresh_)
								headNode->dq_count = DQ_COUNT_INVALID;
							else
							{
								headNode->dq_count = 0;
								//take transmission time into account
								headNode->dq_tstamp = Scheduler::instance().clock() + pktSize * 8 / link_capacity_;
							}

							if (debug_ && marking_scheme_ == PIE_MARKING)
								printf("[queue %d] sample departure rate : %.2f average departure rate: %.2f\n", headNode->id + prio_queue_num_, rate, headNode->avg_dq_rate);
						}
					}

					/* After dequeue, headNode becomes empty. In such case, we should delete this queue from activeList. */
					if (headNode->length() == 0)
					{
						round_time_sample = Scheduler::instance().clock() - headNode->start_time + pktSize * 8 / link_capacity_;
						round_time = round_time * estimate_round_alpha_ + round_time_sample * (1 - estimate_round_alpha_);

						if (debug_ && marking_scheme_ == MQ_MARKING_RR)
							printf("sample round time: %.9f round time: %.9f\n", round_time_sample, round_time);

						quantum_sum -= headNode->quantum;
						headNode = RemoveHeadList(activeList);
						headNode->deficitCounter = 0;
						headNode->active = false;
						headNode->current = false;
					}
					break;
				}
				/* if we don't have enough quantum to dequeue the head packet and the queue is not empty */
				else
				{
					headNode = RemoveHeadList(activeList);
					headNode->current = false;
					round_time_sample = Scheduler::instance().clock() - headNode->start_time;
				  	round_time = round_time * estimate_round_alpha_ + round_time_sample * (1-estimate_round_alpha_);

					if (debug_ && marking_scheme_ == MQ_MARKING_RR)
						printf("sample round time: %.9f round time: %.9f\n",round_time_sample,round_time);

					headNode->start_time = Scheduler::instance().clock();	//Reset start time
					InsertTailList(activeList, headNode);
				}
			}
		}
	}

	if(marking_scheme_ == MQ_MARKING_GENER && !estimate_quantum_enable_timer_)
	{
		double now = Scheduler::instance().clock();
		double timeInterval = now - last_update_time;
		if (estimate_quantum_interval_bytes_ > 0 && link_capacity_ > 0 && timeInterval >= 0.995 * estimate_quantum_interval_bytes_ * 8 /link_capacity_)
		{
			quantum_sum_estimate = quantum_sum_estimate * estimate_quantum_alpha_ + quantum_sum * (1 - estimate_quantum_alpha_);
			last_update_time = now;
			if(debug_)
				printf("%.9f smooth quantum sum: %f, sample quantum sum: %d\n", now, quantum_sum_estimate, quantum_sum);
		}
	}

	if (Total_DWRR_ByteLength() == 0)
		last_idle_time = Scheduler::instance().clock();

	return pkt;
}

/* routine to write total qlen records */
void PRIO_DWRR::trace_total_qlen()
{
	if (total_qlen_tchan_)
	{
		char wrk[500] = {0};
		int n;
		double t = Scheduler::instance().clock();
		sprintf(wrk, "%g, %d", t, TotalByteLength());
		n = strlen(wrk);
		wrk[n] = '\n';
		wrk[n+1] = 0;
		(void)Tcl_Write(total_qlen_tchan_, wrk, n+1);
	}
}

/* routine to write per-queue qlen records */
void PRIO_DWRR::trace_qlen()
{
	if (qlen_tchan_)
	{
		char wrk[500] = {0};
		int n;
		double t = Scheduler::instance().clock();
		sprintf(wrk, "%g", t);
		n = strlen(wrk);
		wrk[n] = 0;
		(void)Tcl_Write(qlen_tchan_, wrk, n);

		for (int i = 0; i < prio_queue_num_; i++)
		{
			sprintf(wrk, ", %d", prio_queues[i].byteLength());
			n = strlen(wrk);
			wrk[n] = 0;
			(void)Tcl_Write(qlen_tchan_, wrk, n);
		}

		for (int i = 0; i < dwrr_queue_num_; i++)
		{
			sprintf(wrk, ", %d", dwrr_queues[i].byteLength());
			n = strlen(wrk);
			wrk[n] = 0;
			(void)Tcl_Write(qlen_tchan_, wrk, n);
		}

		(void)Tcl_Write(qlen_tchan_, "\n", 1);
	}
}
