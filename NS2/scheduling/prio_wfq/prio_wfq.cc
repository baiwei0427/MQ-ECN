#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include "flags.h"
#include "prio_wfq.h"

#define max(arg1,arg2) (arg1>arg2 ? arg1 : arg2)
#define min(arg1,arg2) (arg1<arg2 ? arg1 : arg2)

static class PrioWfqClass : public TclClass
{
    public:
        PrioWfqClass() : TclClass("Queue/PrioWfq") {}
        TclObject* create(int argc, const char*const* argv)
        {
			return (new PRIO_WFQ);
        }
} class_prio_wfq;

PRIO_WFQ::PRIO_WFQ(): timer_(this)
{
    prio_queues = new PacketPRIO[MAX_PRIO_QUEUE_NUM];
	wfq_queues = new PacketWFQ[MAX_WFQ_QUEUE_NUM];

	currTime = 0;
	weight_sum_estimate = 0;
	weight_sum = 0;
	last_update_time = 0;
	last_idle_time = 0;
	init = 0;

    prio_queue_num_ = 1;
	wfq_queue_num_ = 7;
	mean_pktsize_ = 1500;
	port_thresh_ = 65;
	marking_scheme_ = 0;
	estimate_weight_alpha_ = 0.75;
	estimate_weight_interval_bytes_ = 1500;
	estimate_weight_enable_timer_ = 0;
	dq_thresh_ = 10000;
	estimate_rate_alpha_ = 0.875;
	link_capacity_ = 10000000000;
	debug_ = 0;

	total_qlen_tchan_ = NULL;
	qlen_tchan_ = NULL;

	/* bind variables */
	bind("prio_queue_num_", &prio_queue_num_);
    bind("wfq_queue_num_", &wfq_queue_num_);
	bind("mean_pktsize_", &mean_pktsize_);
	bind("port_thresh_", &port_thresh_);
	bind("marking_scheme_", &marking_scheme_);
	bind("estimate_weight_alpha_", &estimate_weight_alpha_);
	bind("estimate_weight_interval_bytes_", &estimate_weight_interval_bytes_);
	bind_bool("estimate_weight_enable_timer_", &estimate_weight_enable_timer_);
	bind("dq_thresh_", &dq_thresh_);
	bind("estimate_rate_alpha_", &estimate_rate_alpha_);
	bind_bw("link_capacity_", &link_capacity_);
	bind_bool("debug_", &debug_);
}

PRIO_WFQ::~PRIO_WFQ()
{
    delete [] prio_queues;
    delete [] wfq_queues;
    timer_.cancel();
}

void PRIO_WFQ::timeout(int)
{
	/* update weight_sum_estimate */
	weight_sum_estimate = weight_sum_estimate * estimate_weight_alpha_ + weight_sum * (1 - estimate_weight_alpha_);

	if (debug_ && marking_scheme_ == MQ_MARKING_GENER)
		printf("%.9f smooth weight sum: %f, sample weight sum: %f\n", Scheduler::instance().clock(), weight_sum_estimate, weight_sum);

	/* reschedule timer */
	if (link_capacity_ > 0)
		timer_.resched(estimate_weight_interval_bytes_ * 8 / link_capacity_);
}

void PRIO_WFQ_Timer::expire(Event* e)
{
	queue_->timeout(0);
}

/* Get total length of all WFQ queues in bytes */
int PRIO_WFQ::Total_WFQ_ByteLength()
{
	int result = 0;

	for (int i = 0; i < MAX_WFQ_QUEUE_NUM; i++)
		result += wfq_queues[i].byteLength();

	return result;
}

/* Get total length of all higher priority queues in bytes */
int PRIO_WFQ::Total_Prio_ByteLength()
{
	int result = 0;

	for (int i = 0; i < MAX_PRIO_QUEUE_NUM; i++)
		result += prio_queues[i].byteLength();

	return result;
}

/* Get total length of all queues in bytes */
int PRIO_WFQ::TotalByteLength()
{
	return Total_WFQ_ByteLength() + Total_Prio_ByteLength();
}

/* Determine whether we need to mark ECN.
 * Return 1 if it requires marking
 */
int PRIO_WFQ::MarkingECN(int queue_index)
{
	int type = 0;
	int prio_queue_index = 0;
	int wfq_queue_index = 0;

	if (queue_index < 0 || queue_index >= prio_queue_num_ + wfq_queue_num_)
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
		wfq_queue_index = queue_index - prio_queue_num_;
		type = WFQ_QUEUE;
	}

	/* Per-queue ECN marking */
	if (marking_scheme_ == PER_QUEUE_MARKING)
	{
		if (type == PRIO_QUEUE && prio_queues[prio_queue_index].byteLength() > prio_queues[prio_queue_index].thresh * mean_pktsize_)
			return 1;
		else if (type == WFQ_QUEUE && wfq_queues[wfq_queue_index].byteLength() > wfq_queues[wfq_queue_index].thresh * mean_pktsize_)
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
		if (type == WFQ_QUEUE)
		{
			double thresh = 0;
			if (weight_sum_estimate >= 0.000000001)
				thresh = min(wfq_queues[wfq_queue_index].weight / weight_sum_estimate, 1) * port_thresh_;
			else
				thresh = port_thresh_;

			if (wfq_queues[wfq_queue_index].byteLength() > thresh * mean_pktsize_)
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

		if (type == WFQ_QUEUE && wfq_queues[wfq_queue_index].avg_dq_rate >= 0.000000001 && link_capacity_ > 0)
			thresh = min(wfq_queues[wfq_queue_index].avg_dq_rate / link_capacity_, 1) * port_thresh_;
		else if (type == PRIO_QUEUE && prio_queues[prio_queue_index].avg_dq_rate >= 0.000000001 && link_capacity_ > 0)
			thresh = min(prio_queues[prio_queue_index].avg_dq_rate / link_capacity_, 1) * port_thresh_;
		else
			thresh = port_thresh_;

		if (type == WFQ_QUEUE && wfq_queues[wfq_queue_index].byteLength() > thresh * mean_pktsize_)
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
 *  - $q set-weight queue_id queue_weight
 *  - $q set-thresh queue_id queue_thresh
 *  - $q attach-total file
 *  - $q attach-queue file
 *
 *  NOTE: $q represents the discipline queue variable in OTcl.
 */
int PRIO_WFQ::command(int argc, const char*const* argv)
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
				tcl.resultf("PRIO_WFQ: trace: can't attach %s for writing", id);
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
				tcl.resultf("PRIO_WFQ: trace: can't attach %s for writing", id);
				return (TCL_ERROR);
			}
			return (TCL_OK);
		}
	}
	else if (argc == 4)
	{
		if (strcmp(argv[1], "set-weight")==0)
		{
			int wfq_queue_index = atoi(argv[2]) - prio_queue_num_;
			if (wfq_queue_index < wfq_queue_num_ && wfq_queue_index >= 0)
			{
				int weight = atoi(argv[3]);
				if (weight > 0)
				{
					wfq_queues[wfq_queue_index].weight = weight;
					return (TCL_OK);
				}
				else
				{
					fprintf(stderr, "illegal weight value %d for WFQ queue %d\n", weight, wfq_queue_index);
					exit(1);
				}
			}
			/* Exceed the maximum queue number or smaller than 0*/
			else
			{
				fprintf(stderr, "illegal WFQ queue index value %d\n", wfq_queue_index);
				exit(1);
			}
		}
		else if (strcmp(argv[1], "set-thresh") == 0)
		{
			int queue_index = atoi(argv[2]);
			if (queue_index < prio_queue_num_ + wfq_queue_num_ && queue_index >= 0)
			{
				double thresh = atof(argv[3]);
				if (thresh >= 0)
				{
					if (queue_index < prio_queue_num_)
						prio_queues[queue_index].thresh = thresh;
					else
						wfq_queues[queue_index - prio_queue_num_].thresh = thresh;
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
void PRIO_WFQ::enque(Packet *p)
{
	hdr_ip *iph = hdr_ip::access(p);
	int prio = iph->prio();
	hdr_flags* hf = hdr_flags::access(p);
	hdr_cmn* hc = hdr_cmn::access(p);
	int pktSize = hc->size();
	int qlimBytes = qlim_ * mean_pktsize_;
    wfq_queue_num_ = max(min(wfq_queue_num_, MAX_WFQ_QUEUE_NUM), 1);
	prio_queue_num_ = max(min(prio_queue_num_, MAX_PRIO_QUEUE_NUM), 1);
	int queue_num_ = wfq_queue_num_ + prio_queue_num_;

	if (init == 0)
	{
		/* Start timer*/
		if (marking_scheme_ == MQ_MARKING_GENER && estimate_weight_enable_timer_ && estimate_weight_interval_bytes_ > 0 && link_capacity_ > 0)
			timer_.resched(estimate_weight_interval_bytes_ * 8 / link_capacity_);
		init = 1;
	}

	if (Total_WFQ_ByteLength() == 0 && marking_scheme_ == MQ_MARKING_GENER && !estimate_weight_enable_timer_)
	{
		double now = Scheduler::instance().clock();
		double idleTime = now-last_idle_time;
		if (estimate_weight_interval_bytes_ > 0 && link_capacity_ > 0)
			weight_sum_estimate = weight_sum_estimate * pow(estimate_weight_alpha_, idleTime / (estimate_weight_interval_bytes_ * 8 / link_capacity_));
		else
			weight_sum_estimate = 0;

		last_update_time = now;
		if(debug_)
			printf("%.9f smooth weight sum is reset to %f\n", now, weight_sum_estimate);
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
        int wfq_queue_index = prio - prio_queue_num_;
        /* If queue for the flow is empty, calculate headFinishTime and currTime */
        if (wfq_queues[wfq_queue_index].length() == 0)
        {
            weight_sum += wfq_queues[wfq_queue_index].weight;
            if (wfq_queues[wfq_queue_index].weight > 0)
            {
                wfq_queues[wfq_queue_index].headFinishTime = currTime + pktSize / wfq_queues[wfq_queue_index].weight ;
                currTime = wfq_queues[wfq_queue_index].headFinishTime;
            }
            /* In theory, weight should never be zero or negative */
            else
		    {
                fprintf(stderr,"enqueue: illegal weight value for queue %d\n", prio);
                exit(1);
		    }
        }
        wfq_queues[wfq_queue_index].enque(p);
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

Packet* PRIO_WFQ::deque(void)
{
	Packet *pkt = NULL, *nextPkt = NULL;
	hdr_flags* hf = NULL;
	hdr_cmn* hc = NULL;
	long double minT = LDBL_MAX ;
	int queue = -1;
	double sojourn_time = 0;
	double latency_thresh = 0;
	int pktSize = 0;

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
							printf("[queue %d] sample departure rate : %.2f average departure rate: %.2f\n", i, rate, prio_queues[i].avg_dq_rate);
					}
				}
				break;
			}
		}
		return pkt;
    }
	else if (Total_WFQ_ByteLength() > 0)
	{
		/* look for the candidate queue with the earliest virtual finish time */
		for (int i = 0; i < wfq_queue_num_; i++)
		{
			if (wfq_queues[i].length() == 0)
				continue;

			if (wfq_queues[i].headFinishTime < minT)
			{
				queue = i;
				minT = wfq_queues[i].headFinishTime;
			}
		}

		if (queue == -1 && Total_WFQ_ByteLength() > 0)
		{
			fprintf(stderr,"not work conserving\n");
			exit(1);
		}

		pkt = wfq_queues[queue].deque();
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
		if (wfq_queues[queue].byteLength() >= dq_thresh_ && wfq_queues[queue].dq_count == DQ_COUNT_INVALID)
		{
			wfq_queues[queue].dq_tstamp = Scheduler::instance().clock();
			wfq_queues[queue].dq_count = 0;
		}

		/* Calculate the average drain rate from this value.  If queue length
		 * has receded to a small value viz., <= dq_thresh_bytes,reset
		 * the dq_count to -1 as we don't have enough packets to calculate the
		 * drain rate anymore The following if block is entered only when we
		 * have a substantial queue built up (dq_thresh_ bytes or more)
		 * and we calculate the drain rate for the threshold here.*/
		if (wfq_queues[queue].dq_count != DQ_COUNT_INVALID)
		{
			wfq_queues[queue].dq_count += pktSize;
			if (wfq_queues[queue].dq_count >= dq_thresh_)
			{
				//take transmission time into account
				double interval = Scheduler::instance().clock() - wfq_queues[queue].dq_tstamp + pktSize * 8 / link_capacity_;
				double rate = wfq_queues[queue].dq_count * 8 / interval;

				//initialize avg_dq_rate for this queue
				if (wfq_queues[queue].avg_dq_rate < 0)
					wfq_queues[queue].avg_dq_rate = rate;
				else
					wfq_queues[queue].avg_dq_rate = wfq_queues[queue].avg_dq_rate * estimate_rate_alpha_ + rate * (1 - estimate_rate_alpha_);

				/* If the queue has receded below the threshold, we hold
				 * on to the last drain rate calculated, else we reset
				 * dq_count to 0 to re-enter the if block when the next
				 * packet is dequeued
				 */
				if (wfq_queues[queue].byteLength() < dq_thresh_)
					wfq_queues[queue].dq_count = DQ_COUNT_INVALID;
				else
				{
					wfq_queues[queue].dq_count = 0;
					//take transmission time into account
					wfq_queues[queue].dq_tstamp = Scheduler::instance().clock() + pktSize * 8 / link_capacity_;
				}

				if (debug_ && marking_scheme_ == PIE_MARKING)
					printf("[queue %d] sample departure rate : %.2f average departure rate: %.2f\n", queue + prio_queue_num_, rate, wfq_queues[queue].avg_dq_rate);
			}
		}

		/* Set the headFinishTime for the remaining head packet in the queue */
		nextPkt = wfq_queues[queue].head();
		if (nextPkt != NULL)
		{
			if (wfq_queues[queue].weight > 0)
			{
				wfq_queues[queue].headFinishTime = wfq_queues[queue].headFinishTime + (hdr_cmn::access(nextPkt)->size()) / wfq_queues[queue].weight;
				if (currTime < wfq_queues[queue].headFinishTime)
					currTime = wfq_queues[queue].headFinishTime;
			}
			else
			{
				fprintf(stderr,"dequeue: illegal weight value\n");
				exit(1);
			}
		}
		/* After dequeue, the queue becomes empty */
		else
		{
			weight_sum -= wfq_queues[queue].weight;
			wfq_queues[queue].headFinishTime = LDBL_MAX;
		}
	}

	if (marking_scheme_ == MQ_MARKING_GENER && !estimate_weight_enable_timer_)
	{
		double now = Scheduler::instance().clock();
		double timeInterval = now - last_update_time;
		if (estimate_weight_interval_bytes_ > 0 && link_capacity_ > 0 && timeInterval >= 0.995 * estimate_weight_interval_bytes_ * 8 / link_capacity_)
		{
			weight_sum_estimate = weight_sum_estimate * estimate_weight_alpha_ + weight_sum * (1 - estimate_weight_alpha_);
			last_update_time = now;
			if(debug_)
				printf("%.9f smooth weight sum: %f, sample weight sum: %f\n", now, weight_sum_estimate, weight_sum);
		}

		if (Total_WFQ_ByteLength() == 0)
			last_idle_time = now;
	}

	return pkt;
}

/* routine to write total qlen records */
void PRIO_WFQ::trace_total_qlen()
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
void PRIO_WFQ::trace_qlen()
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

		for (int i = 0; i < wfq_queue_num_; i++)
		{
			sprintf(wrk, ", %d", wfq_queues[i].byteLength());
			n = strlen(wrk);
			wrk[n] = 0;
			(void)Tcl_Write(qlen_tchan_, wrk, n);
		}

		(void)Tcl_Write(qlen_tchan_, "\n", 1);
	}
}
