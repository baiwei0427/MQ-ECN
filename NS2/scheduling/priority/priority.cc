/*
 * Strict Priority Queueing (SP)
 *
 * Variables:
 * queue_num_: number of CoS queues
 * thresh_: ECN marking threshold
 * mean_pktsize_: configured mean packet size in bytes
 * marking_scheme_: Per-queue ECN (0), Per-port ECN (1) and Latency ECN marking (4)
 */

#include "priority.h"
#include "flags.h"
#include "math.h"

#define max(arg1,arg2) (arg1>arg2 ? arg1 : arg2)
#define min(arg1,arg2) (arg1<arg2 ? arg1 : arg2)

static class PriorityClass : public TclClass {
 public:
	PriorityClass() : TclClass("Queue/Priority") {}
	TclObject* create(int, const char*const*) {
		return (new Priority);
	}
} class_priority;

Priority::Priority()
{
    queue_num_ = MAX_QUEUE_NUM;
    thresh_ = 65;
    mean_pktsize_ = 1500;
    marking_scheme_ = PER_QUEUE_MARKING;
    dq_thresh_ = 10000;
    estimate_rate_alpha_ = 0.875;
    link_capacity_ = 10000000000;   //10Gbps
    debug_ = 0;

    total_qlen_tchan_ = NULL;
    qlen_tchan_ = NULL;

    //Bind variables
    bind("queue_num_", &queue_num_);
    bind("thresh_", &thresh_);
    bind("mean_pktsize_", &mean_pktsize_);
    bind("marking_scheme_", &marking_scheme_);
    bind("dq_thresh_", &dq_thresh_);
    bind("estimate_rate_alpha_", &estimate_rate_alpha_);
    bind_bw("link_capacity_", &link_capacity_);
    bind_bool("debug_", &debug_);

    //Init queues and per-queue variables
    queues = new PacketQueue[MAX_QUEUE_NUM];
    dq_tstamps = new double[MAX_QUEUE_NUM];
    dq_counts = new int[MAX_QUEUE_NUM];
    avg_dq_rates = new double[MAX_QUEUE_NUM];

    if (!queues || !dq_tstamps || !dq_counts || !avg_dq_rates)
        fprintf(stderr, "New Error\n");

    for (int i = 0; i < MAX_QUEUE_NUM; i++)
    {
        dq_tstamps[i] = 0;
        dq_counts[i] = DQ_COUNT_INVALID;
        avg_dq_rates[i] = -1;
    }

}

Priority::~Priority()
{
    delete[] queues;
    delete[] dq_tstamps;
    delete[] dq_counts;
    delete[] avg_dq_rates;
}

int Priority::TotalByteLength()
{
    int bytelength = 0;

    for (int i = 0; i < MAX_QUEUE_NUM; i++)
        bytelength += queues[i].byteLength();

    return bytelength;
}

/* Determine whether we need to mark ECN where q is current queue number. Return 1 if it requires marking */
int Priority::MarkingECN(int q)
{
	if (q < 0 || q >= queue_num_)
	{
		fprintf (stderr,"illegal queue number\n");
		exit (1);
	}

	/* Per-queue ECN marking */
	if (marking_scheme_ == PER_QUEUE_MARKING)
	{
		if (queues[q].byteLength() > thresh_ * mean_pktsize_)
			return 1;
		else
			return 0;
	}
	/* Per-port ECN marking */
	else if (marking_scheme_ == PER_PORT_MARKING)
	{
		if (TotalByteLength() > thresh_ * mean_pktsize_)
			return 1;
		else
			return 0;
	}
	/* PIE-like ECN marking */
	else if (marking_scheme_ == PIE_MARKING)
	{
		double thresh = 0;
		if (avg_dq_rates[q] > 0 && link_capacity_ > 0)
			thresh = min(avg_dq_rates[q] / link_capacity_, 1) * thresh_;
		else
			thresh = thresh_;

		if (queues[q].byteLength() > thresh * mean_pktsize_)
			return 1;
		else
			return 0;
	}
	/* Unknown ECN marking scheme */
	else
	{
		fprintf(stderr,"Unknown ECN marking scheme\n");
		return 0;
	}
}

void Priority::enque(Packet* p)
{
	hdr_ip *iph = hdr_ip::access(p);
	int prio = iph->prio();
	hdr_flags* hf = hdr_flags::access(p);
    hdr_cmn* hc = hdr_cmn::access(p);
    int pktSize = hc->size();
	int qlimBytes = qlim_ * mean_pktsize_;

    //1 <= queue_num_ <= MAX_QUEUE_NUM
    queue_num_ = max(min(queue_num_, MAX_QUEUE_NUM), 1);

	//queue length exceeds the queue limit
	if (TotalByteLength() + pktSize > qlimBytes)
	{
		drop(p);
		return;
	}

    if (prio >= queue_num_ || prio < 0)
		prio = queue_num_ - 1;
	//Enqueue packet
	queues[prio].enque(p);

	/* Enqueue ECN marking */
    if (marking_scheme_ != LATENCY_MARKING && MarkingECN(prio) > 0 && hf->ect())
		hf->ce() = 1;
	/* For dequeue latency ECN marking ,record enqueue timestamp here */
	else if (marking_scheme_ == LATENCY_MARKING && hf->ect())
		hc->timestamp() = Scheduler::instance().clock();

    trace_qlen();
    trace_total_qlen();
}

Packet* Priority::deque()
{
    Packet* p = NULL;
    hdr_flags* hf = NULL;
    hdr_cmn* hc = NULL;
    double sojourn_time = 0;
    double latency_thresh = 0;
    int pktSize = 0;

    if (TotalByteLength() > 0)
	{
        //high->low: 0->7
	    for (int i = 0; i < queue_num_; i++)
	    {
		    if (queues[i].length() > 0)
            {
			    p = queues[i].deque();
                pktSize = hdr_cmn::access(p)->size();

                if (marking_scheme_ == LATENCY_MARKING)
                {
                    hc = hdr_cmn::access(p);
                    hf = hdr_flags::access(p);
                    sojourn_time = Scheduler::instance().clock() - hc->timestamp();
                    if (link_capacity_ > 0)
                        latency_thresh = thresh_ * mean_pktsize_ * 8 / link_capacity_;

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
                if (queues[i].byteLength() >= dq_thresh_ && dq_counts[i] == DQ_COUNT_INVALID)
                {
                    dq_tstamps[i] = Scheduler::instance().clock();
                    dq_counts[i] = 0;
                }

                /* Calculate the average drain rate from this value.  If queue length
                 * has receded to a small value viz., <= dq_thresh_bytes,reset
                 * the dq_count to -1 as we don't have enough packets to calculate the
                 * drain rate anymore The following if block is entered only when we
                 * have a substantial queue built up (dq_thresh_ bytes or more)
                 * and we calculate the drain rate for the threshold here.*/
                if (dq_counts[i] != DQ_COUNT_INVALID)
                {
                    dq_counts[i] += pktSize;
                    if (dq_counts[i] >= dq_thresh_)
                    {
                        //take transmission time into account
                        double interval = Scheduler::instance().clock() - dq_tstamps[i] + pktSize * 8 / link_capacity_;
                        double rate = dq_counts[i] * 8 / interval;

                        //initialize avg_dq_rate for this queue
                        if (avg_dq_rates[i] < 0)
                            avg_dq_rates[i] = rate;
                        else
                            avg_dq_rates[i] = avg_dq_rates[i] * estimate_rate_alpha_ + rate * (1 - estimate_rate_alpha_);

                        /* If the queue has receded below the threshold, we hold
                         * on to the last drain rate calculated, else we reset
                         * dq_count to 0 to re-enter the if block when the next
                         * packet is dequeued
                         */
                        if (queues[i].byteLength() < dq_thresh_)
                            dq_counts[i] = DQ_COUNT_INVALID;
                        else
                        {
                            dq_counts[i] = 0;
                            //take transmission time into account
                            dq_tstamps[i] = Scheduler::instance().clock() + pktSize * 8 / link_capacity_;
                        }

                        if (debug_ && marking_scheme_ == PIE_MARKING)
                            printf("[queue %d] sample departure rate : %.2f average departure rate: %.2f\n", i, rate, avg_dq_rates[i]);
                    }
                }
		        return (p);
		    }
        }
    }

	return NULL;
}

/*
 *  entry points from OTcL to set per queue state variables
 *   - $q attach-total file
 *	 - $q attach-queue file
 *
 *  NOTE: $q represents the discipline queue variable in OTcl.
 */
int Priority::command(int argc, const char*const* argv)
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
				tcl.resultf("Priority: trace: can't attach %s for writing", id);
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
				tcl.resultf("Priority: trace: can't attach %s for writing", id);
				return (TCL_ERROR);
			}
			return (TCL_OK);
		}
	}
	return (Queue::command(argc, argv));
}

/* routine to write total qlen records */
void Priority::trace_total_qlen()
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
void Priority::trace_qlen()
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

		for (int i = 0; i < queue_num_; i++)
		{
			sprintf(wrk, ", %d", queues[i].byteLength());
			n = strlen(wrk);
			wrk[n] = 0;
			(void)Tcl_Write(qlen_tchan_, wrk, n);
		}
		(void)Tcl_Write(qlen_tchan_, "\n", 1);
	}
}
