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
    link_capacity_ = 10000000000;   //10Gbps
    debug_ = 0;

    total_qlen_tchan_ = NULL;
    qlen_tchan_ = NULL;

    //Bind variables
    bind("queue_num_", &queue_num_);
    bind("thresh_", &thresh_);
    bind("mean_pktsize_", &mean_pktsize_);
    bind("marking_scheme_", &marking_scheme_);
    bind_bw("link_capacity_", &link_capacity_);
    bind_bool("debug_", &debug_);

    //Init queues
    queues = new PacketQueue[MAX_QUEUE_NUM];
    if (queues == NULL)
        fprintf(stderr, "New Error\n");
}

Priority::~Priority()
{
    delete[] queues;
}

int Priority::TotalByteLength()
{
    int bytelength = 0;

    for (int i = 0; i < MAX_QUEUE_NUM; i++)
        bytelength += queues[i].byteLength();

    return bytelength;
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

    //Enqueue ECN marking: Per-queue or Per-port
    if (hf->ect() && ((marking_scheme_ == PER_QUEUE_MARKING && queues[prio].byteLength() > thresh_ * mean_pktsize_) ||
    (marking_scheme_ == PER_PORT_MARKING && TotalByteLength() > thresh_ * mean_pktsize_)))
        hf->ce() = 1;
    //Dequeue latency-based ECN marking
    else if (hf->ect() && marking_scheme_ == LATENCY_MARKING)
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

    if (TotalByteLength() > 0)
	{
        //high->low: 0->7
	    for (int i = 0; i < queue_num_; i++)
	    {
		    if (queues[i].length() > 0)
            {
			    p = queues[i].deque();
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
		sprintf(wrk, "%g, %d", t,TotalByteLength());
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
