#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <net/netlink.h>
#include <linux/pkt_sched.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
#include <linux/ip.h>
#include <net/dsfield.h>
#include <net/inet_ecn.h>

#include "params.h"


struct dwrr_rate_cfg
{
	u64 rate_bps;
	u32 mult;
	u32 shift;
};

/**
 *	struct dwrr_class - a Class of Service (CoS) queue
 *	@id: queue ID
 *	@qdisc: FIFO queue to store sk_buff
 *
 *	For DWRR scheduling
 *	@deficit: deficit counter of this queue (bytes)
 *	@active: whether the queue is not ampty (1) or not (0)
 *  @curr: whether this queue is currently being served (head of the linked list)
 *	@len_bytes: queue length in bytes
 *  @start_time_ns: time when this queue is inserted to active list
 *	@last_pkt_time_ns: time when this queue transmits the last packet
 *	@last_pkt_len_ns: length of last packet/rate
 *	@quantum: quantum in bytes of this queue
 *  @alist: active linked list
 *
 *	For CoDel
 *	@count: how many marks we've done since the last time we entered marking/dropping state
 *	@lastcount: count at entry to marking/dropping state
 *	@marking: equal to true if in mark/drop state
 *	@rec_inv_sqrt: reciprocal value of sqrt(count) >> 1
 *	@first_above_time: when we went (or will go) continuously above target for interval
 *	@mark_next: time to mark/drop next packet, or when we marked/dropped last
 *	@ldelay: sojourn time of last dequeued packet
 */
struct dwrr_class
{
	int id;
	struct Qdisc *qdisc;

	u32	deficit;
	u8 active;
	u8 curr;
	u32 len_bytes;
	s64 start_time_ns;
	s64 last_pkt_time_ns;
	s64 last_pkt_len_ns;
	u32 quantum;
	struct list_head alist;

	u32 count;
	u32 lastcount;
	bool marking;
	u16 rec_inv_sqrt;
    codel_time_t first_above_time;
	codel_time_t mark_next;
	codel_time_t ldelay;
};

/**
 *	struct dwrr_sched_data - DWRR scheduler
 *	@queues: multiple Class of Service (CoS) queues
 *	@rate: shaping rate
 *	@active_list: linked list to store active queues
 *
 *	@tokens: tokens in ns
 *	@sum_len_bytes: the total buffer occupancy (in bytes) of the switch port
 *	@time_ns: time check-point
 *	@watchdog: watchdog timer for token bucket rate limiter
 *	@round_time_ns: estimation of round time in ns
 *	@last_idle_time_ns: last time when the port is idle
 *	@quantum_sum: sum of quantum of all active queues
 *	@quantum_sum_estimate: estimation of quantum_sum
 */
struct dwrr_sched_data
{
	struct dwrr_class *queues;
	struct dwrr_rate_cfg rate;
	struct list_head active_list;

	s64 tokens;
	u32 sum_len_bytes;
	s64	time_ns;
	struct qdisc_watchdog watchdog;
	s64 round_time_ns;
	s64 last_idle_time_ns;
	u32 quantum_sum;
	u32 quantum_sum_estimate;
};

/*
 * We use this function to account for the true number of bytes sent on wire.
 * 20 = frame check sequence(8B) + Interpacket gap(12B)
 * 4 = Frame check sequence (4B)
 * DWRR_QDISC_MIN_PKT_BYTES = Minimum Ethernet frame size (64B)
 */
static inline unsigned int skb_size(struct sk_buff *skb)
{
	return max_t(unsigned int, skb->len + 4, DWRR_QDISC_MIN_PKT_BYTES) + 20;
}

/* Borrow from ptb */
static inline void precompute_ratedata(struct dwrr_rate_cfg *r)
{
	r->shift = 0;
	r->mult = 1;

	if (r->rate_bps > 0)
	{
		r->shift = 15;
		r->mult = div64_u64(8LLU * NSEC_PER_SEC * (1 << r->shift), r->rate_bps);
	}
}

/* Borrow from ptb: length (bytes) to time (nanosecond) */
static inline u64 l2t_ns(struct dwrr_rate_cfg *r, unsigned int len_bytes)
{
	return ((u64)len_bytes * r->mult) >> r->shift;
}

static inline codel_time_t ns_to_codel_time(s64 ns)
{
    return ns >> CODEL_SHIFT;
}

/* TCN marking scheme */
static bool tcn_marking(const struct sk_buff *skb)
{
    if (codel_time_after(ns_to_codel_time(ktime_get_ns() - skb->tstamp.tv64), (codel_time_t)DWRR_QDISC_TCN_THRESH))
		return true;
	else
		return false;
}

/* Borrow from codel_should_drop in Linux kernel */
static bool codel_should_mark(const struct sk_buff *skb, struct dwrr_class *cl, s64 now)
{
    bool ok_to_mark;
	cl->ldelay = ns_to_codel_time(now - skb->tstamp.tv64);

	if (codel_time_before(cl->ldelay, (codel_time_t)DWRR_QDISC_CODEL_TARGET) ||
        cl->len_bytes <= DWRR_QDISC_MTU_BYTES)
	{
		/* went below - stay below for at least interval */
		cl->first_above_time = 0;
		return false;
	}

    ok_to_mark = false;
	if (cl->first_above_time == 0)
	{
		/* just went above from below. If we stay above
         * for at least interval we'll say it's ok to mark
         */
		cl->first_above_time = ns_to_codel_time(now) + DWRR_QDISC_CODEL_INTERVAL;
	}
	else if (codel_time_after(ns_to_codel_time(now), cl->first_above_time))
	{
        ok_to_mark = true;
	}

    return ok_to_mark;
}

#define REC_INV_SQRT_BITS (8 * sizeof(u16)) /* or sizeof_in_bits(rec_inv_sqrt) */
/* needed shift to get a Q0.32 number from rec_inv_sqrt */
#define REC_INV_SQRT_SHIFT (32 - REC_INV_SQRT_BITS)

/*
 * http://en.wikipedia.org/wiki/Methods_of_computing_square_roots#Iterative_methods_for_reciprocal_square_roots
 * new_invsqrt = (invsqrt / 2) * (3 - count * invsqrt^2)
 *
 * Here, invsqrt is a fixed point number (< 1.0), 32bit mantissa, aka Q0.32
 *
 * Borrow from codel_Newton_step in Linux kernel
 */
static void codel_Newton_step(struct dwrr_class *cl)
{
    u32 invsqrt = ((u32)cl->rec_inv_sqrt) << REC_INV_SQRT_SHIFT;
    u32 invsqrt2 = ((u64)invsqrt * invsqrt) >> 32;
    u64 val = (3LL << 32) - ((u64)cl->count * invsqrt2);

    val >>= 2; /* avoid overflow in following multiply */
    val = (val * invsqrt) >> (32 - 2 + 1);

    cl->rec_inv_sqrt = val >> REC_INV_SQRT_SHIFT;
}

/*
 * CoDel control_law is t + interval/sqrt(count)
 * We maintain in rec_inv_sqrt the reciprocal value of sqrt(count) to avoid
 * both sqrt() and divide operation.
 *
 * Borrow from codel_control_law in Linux kernel
 */
static codel_time_t codel_control_law(codel_time_t t, codel_time_t interval, u32 rec_inv_sqrt)
{
    return t + reciprocal_scale(interval, rec_inv_sqrt << REC_INV_SQRT_SHIFT);
}

/* Implement CoDel ECN marking algorithm. Borrow from codel_dequeue in Linux kernel */
static bool codel_marking(const struct sk_buff *skb, struct dwrr_class *cl)
{
    s64 now = ktime_get_ns();
    bool mark = codel_should_mark(skb, cl, now);

    if (cl->marking)
    {
        if (!mark)
        {
            /* sojourn time below target - leave marking state */
            cl->marking = false;
            return false;
        }
        else if (codel_time_after_eq(ns_to_codel_time(now), cl->mark_next))
        {
            /* It's time for the next mark */
            cl->count++;
            codel_Newton_step(cl);
            cl->mark_next = codel_control_law(cl->mark_next, DWRR_QDISC_CODEL_INTERVAL, cl->rec_inv_sqrt);
            return true;
        }
    }
    else if (mark)
    {
        u32 delta;
        cl->marking = true;
        /* if min went above target close to when we last went below it
         * assume that the drop rate that controlled the queue on the
         * last cycle is a good starting point to control it now.
         */
        delta = cl->count - cl->lastcount;
        if (delta > 1
            && codel_time_before(ns_to_codel_time(now) - cl->mark_next, (codel_time_t)DWRR_QDISC_CODEL_INTERVAL << 4))
        {
            cl->count = delta;
            /* we dont care if rec_inv_sqrt approximation
             * is not very precise :
             * Next Newton steps will correct it quadratically.
             */
            codel_Newton_step(cl);
        }
        else
        {
            cl->count = 1;
            cl->rec_inv_sqrt = ~0U >> REC_INV_SQRT_SHIFT;
        }
        cl->lastcount = cl->count;
        cl->mark_next = codel_control_law(ns_to_codel_time(now), DWRR_QDISC_CODEL_INTERVAL, cl->rec_inv_sqrt);
        return true;
    }

    return false;
}


static struct dwrr_class *dwrr_qdisc_classify(struct sk_buff *skb, struct Qdisc *sch)
{
	int i = 0;
	struct dwrr_sched_data *q = qdisc_priv(sch);
	struct iphdr* iph = ip_hdr(skb);
	int dscp;

	if (unlikely(!(q->queues)))
		return NULL;

	/* Return queue[0] by default*/
	if (unlikely(!iph))
		return &(q->queues[0]);

	dscp = (const int)(iph->tos >> 2);

	for (i = 0; i < DWRR_QDISC_MAX_QUEUES; i++)
	{
		if (dscp == DWRR_QDISC_QUEUE_DSCP[i])
			return &(q->queues[i]);
	}

	return &(q->queues[0]);
}

/* We don't need this */
static struct sk_buff* dwrr_qdisc_peek(struct Qdisc *sch)
{
	return NULL;
}

static struct sk_buff* dwrr_qdisc_dequeue(struct Qdisc *sch)
{
	struct dwrr_sched_data *q = qdisc_priv(sch);
	struct dwrr_class *cl = NULL;
	struct sk_buff *skb = NULL;
	s64 sample_ns = 0;
	unsigned int len;

	/* No active queue */
	if (list_empty(&q->active_list))
		return NULL;

	while (1)
	{
		cl = list_first_entry(&q->active_list, struct dwrr_class, alist);
		if (unlikely(!cl))
			return NULL;

		/* update deficit counter for this round*/
		if (cl->curr == 0)
		{
			cl->curr = 1;
			cl->deficit += cl->quantum;
		}

		/* get head packet */
		skb = cl->qdisc->ops->peek(cl->qdisc);
		if (unlikely(!skb))
		{
			qdisc_warn_nonwc(__func__, cl->qdisc);
			return NULL;
		}

		len = skb_size(skb);
		if (unlikely(len > DWRR_QDISC_MTU_BYTES))
			printk(KERN_INFO "Error: packet length %u is larger than MTU\n", len);

		/* If this packet can be scheduled by DWRR */
		if (len <= cl->deficit)
		{
			s64 now = ktime_get_ns();
			s64 toks = min_t(s64, now - q->time_ns, (s64)l2t_ns(&q->rate, DWRR_QDISC_BUCKET_BYTES)) + q->tokens;
			s64 pkt_ns = (s64)l2t_ns(&q->rate, len);

			/* If we have enough tokens to release this packet */
			if (toks > pkt_ns)
			{
				skb = qdisc_dequeue_peeked(cl->qdisc);
				if (unlikely(!skb))
					return NULL;

				/* Print necessary information in debug mode */
				if (DWRR_QDISC_DEBUG_MODE == DWRR_QDISC_DEBUG_ON)
				{
					printk(KERN_INFO "total buffer occupancy %u\n",q->sum_len_bytes);
					printk(KERN_INFO "queue %d buffer occupancy %u\n",cl->id, cl->len_bytes);
				}

				q->sum_len_bytes -= len;
				sch->q.qlen--;
				cl->len_bytes -= len;
				cl->deficit -= len;
				cl->last_pkt_len_ns = pkt_ns;
				cl->last_pkt_time_ns = ktime_get_ns();

				if (cl->qdisc->q.qlen == 0)
				{
					cl->active = 0;
					cl->curr = 0;
					list_del(&cl->alist);
					q->quantum_sum -= cl->quantum;
					sample_ns = max_t(s64, cl->last_pkt_time_ns - cl->start_time_ns, cl->last_pkt_len_ns);
					q->round_time_ns = (DWRR_QDISC_ROUND_ALPHA * q->round_time_ns + (1024 - DWRR_QDISC_ROUND_ALPHA) * sample_ns) >> 10;

					/* Get start time of idle period */
					if (q->sum_len_bytes == 0)
						q->last_idle_time_ns = ktime_get_ns();

					/* Print necessary information in debug mode with MQ-ECN-RR*/
					if (DWRR_QDISC_DEBUG_MODE == DWRR_QDISC_DEBUG_ON
                        && DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_MQ_ECN_RR)
					{
						printk(KERN_INFO "sample round time %llu \n",sample_ns);
						printk(KERN_INFO "round time %llu\n",q->round_time_ns);
					}
				}

				/* Update quantum_sum_estimate */
				q->quantum_sum_estimate = (DWRR_QDISC_QUANTUM_ALPHA * q->quantum_sum_estimate + (1024 - DWRR_QDISC_QUANTUM_ALPHA) * q->quantum_sum) >> 10;
				/* Print necessary information in debug mode with MQ-ECN-GENER*/
				if (DWRR_QDISC_DEBUG_MODE == DWRR_QDISC_DEBUG_ON && DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_MQ_ECN_GENER)
				{
					printk(KERN_INFO "sample quantum sum %u\n", q->quantum_sum);
					printk(KERN_INFO "quantum sum %u\n", q->quantum_sum_estimate);
				}

				/* Latency-based ECN marking */
				if (skb->tstamp.tv64 > 0)
                {
                    /* TCN */
                    if (DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_TCN
                        && tcn_marking(skb))
					{
                        INET_ECN_set_ce(skb);
                    }
                    /* CoDel */
                    else if (DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_CODEL
                        && codel_marking(skb, cl))
                    {
                        INET_ECN_set_ce(skb);
                    }
				}

				//printk(KERN_INFO "Dequeue from queue %d\n",cl->id);
				/* Bucket */
				q->time_ns = now;
				q->tokens = min_t(s64, toks - pkt_ns, (s64)l2t_ns(&q->rate, DWRR_QDISC_BUCKET_BYTES));
				qdisc_unthrottled(sch);
				qdisc_bstats_update(sch, skb);
				return skb;
			}
			/* if we don't have enough tokens to realse this packet */
			else
			{
				/* we use now+t due to absolute mode of hrtimer (HRTIMER_MODE_ABS) */
				qdisc_watchdog_schedule_ns(&q->watchdog, now + pkt_ns - toks, true);
				qdisc_qstats_overlimit(sch);
				return NULL;
			}
		}
		/* This packet can not be scheduled by DWRR */
		else
		{
			cl->curr = 0;
			sample_ns = max_t(s64, cl->last_pkt_time_ns - cl->start_time_ns, cl->last_pkt_len_ns);
			q->round_time_ns = (DWRR_QDISC_ROUND_ALPHA * q->round_time_ns + (1024 - DWRR_QDISC_ROUND_ALPHA) * sample_ns) >> 10;
			cl->start_time_ns = ktime_get_ns();
			q->quantum_sum -= cl->quantum;
			cl->quantum = DWRR_QDISC_QUEUE_QUANTUM[cl->id];
			q->quantum_sum += cl->quantum;
			list_move_tail(&cl->alist, &q->active_list);

			/* Print necessary information in debug mode with MQ-ECN-RR */
			if (DWRR_QDISC_DEBUG_MODE == DWRR_QDISC_DEBUG_ON && DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_MQ_ECN_RR)
			{
				printk(KERN_INFO "sample round time %llu\n",sample_ns);
				printk(KERN_INFO "round time %llu\n",q->round_time_ns);
			}
		}
	}

	return NULL;
}

static int dwrr_qdisc_enqueue(struct sk_buff *skb, struct Qdisc *sch)
{
    struct dwrr_class *cl = NULL;
    unsigned int len = skb_size(skb);
    struct dwrr_sched_data *q = qdisc_priv(sch);
    int ret;
    u64 estimate_rate_bps = 0;
    u64 ecn_thresh_bytes = 0;
    s64 interval = ktime_get_ns() - q->last_idle_time_ns;
    s64 interval_num = 0;
	int i = 0;

	if (q->sum_len_bytes == 0
        && (DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_MQ_ECN_RR
            || DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_MQ_ECN_GENER))
	{
		if (DWRR_QDISC_IDLE_INTERVAL_NS > 0)
		{
			interval_num = div_s64(interval, DWRR_QDISC_IDLE_INTERVAL_NS);
			if (interval_num <= DWRR_QDISC_MAX_ITERATION)
			{
				for (i = 0; i < interval_num; i++)
				{
					q->round_time_ns = (q->round_time_ns * DWRR_QDISC_ROUND_ALPHA) >> 10;
					q->quantum_sum_estimate = (q->quantum_sum_estimate * DWRR_QDISC_QUANTUM_ALPHA) >> 10;
				}
			}
			else
			{
				q->round_time_ns = 0;
				q->quantum_sum_estimate = 0;
			}
		}
		else
		{
			q->round_time_ns = 0;
			q->quantum_sum_estimate = 0;
		}
		if (DWRR_QDISC_DEBUG_MODE == DWRR_QDISC_DEBUG_ON)
		{
			if (DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_MQ_ECN_RR)
				printk(KERN_INFO "round time is set to %llu\n", q->round_time_ns);
			else
				printk(KERN_INFO "quantum sum is reset to %u\n", q->quantum_sum_estimate);
		}
	}

	cl = dwrr_qdisc_classify(skb,sch);
	/* No appropriate queue or per port shared buffer is overfilled or per queue static buffer is overfilled */
	if (!cl
        || (DWRR_QDISC_BUFFER_MODE == DWRR_QDISC_SHARED_BUFFER
            && q->sum_len_bytes + len > DWRR_QDISC_SHARED_BUFFER_BYTES)
        || (DWRR_QDISC_BUFFER_MODE == DWRR_QDISC_STATIC_BUFFER
            && cl->len_bytes + len > DWRR_QDISC_QUEUE_BUFFER_BYTES[cl->id]))
	{
		qdisc_qstats_drop(sch);
		qdisc_qstats_drop(cl->qdisc);
		kfree_skb(skb);
		return NET_XMIT_DROP;
	}
	else
	{
		ret = qdisc_enqueue(skb, cl->qdisc);
		if (ret == NET_XMIT_SUCCESS)
		{
            if (cl->len_bytes == 0
                && DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_CODEL)
            {
                /* CoDel leaves marking state when the queue is empty */
                cl->marking = false;
            }

			/* Update queue sizes */
			sch->q.qlen++;
			q->sum_len_bytes += len;
			cl->len_bytes += len;

			if (cl->active == 0)
			{
				cl->deficit = 0;
				cl->active = 1;
				cl->curr = 0;
				cl->start_time_ns = ktime_get_ns();
				cl->quantum = DWRR_QDISC_QUEUE_QUANTUM[cl->id];
				list_add_tail(&(cl->alist), &(q->active_list));
				q->quantum_sum += cl->quantum;
			}

			/* Per-queue ECN marking */
			if (DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_QUEUE_ECN && cl->len_bytes > DWRR_QDISC_QUEUE_THRESH_BYTES[cl->id])
			{
				//printk(KERN_INFO "ECN marking\n");
				INET_ECN_set_ce(skb);
			}
			/* Per-port ECN marking */
			else if (DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_PORT_ECN && q->sum_len_bytes > DWRR_QDISC_PORT_THRESH_BYTES)
			{
				INET_ECN_set_ce(skb);
			}
			/* MQ-ECN for any packet scheduling algorithm */
			else if (DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_MQ_ECN_GENER)
			{
				if (q->quantum_sum_estimate > 0)
					ecn_thresh_bytes = min_t(u64, div_u64((u64)cl->quantum * DWRR_QDISC_PORT_THRESH_BYTES, q->quantum_sum_estimate), DWRR_QDISC_PORT_THRESH_BYTES);
				else
                    ecn_thresh_bytes = DWRR_QDISC_PORT_THRESH_BYTES;

				if (cl->len_bytes > ecn_thresh_bytes)
					INET_ECN_set_ce(skb);

				if (DWRR_QDISC_DEBUG_MODE == DWRR_QDISC_DEBUG_ON)
					printk(KERN_INFO "queue %d quantum %u ECN threshold %llu\n", cl->id, cl->quantum, ecn_thresh_bytes);
			}
			/* MQ-ECN for round robin algorithms */
			else if (DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_MQ_ECN_RR)
			{
				if (q->round_time_ns > 0)
				{
                    estimate_rate_bps = min_t(u64, div_u64((u64)cl->quantum << 33, q->round_time_ns), q->rate.rate_bps);
                    ecn_thresh_bytes =  div_u64(estimate_rate_bps * DWRR_QDISC_PORT_THRESH_BYTES, q->rate.rate_bps);
                }
				else
				{
                    ecn_thresh_bytes = DWRR_QDISC_PORT_THRESH_BYTES;
                }

				if (cl->len_bytes > ecn_thresh_bytes)
					INET_ECN_set_ce(skb);

				if (DWRR_QDISC_DEBUG_MODE == DWRR_QDISC_DEBUG_ON)
					printk(KERN_INFO "queue %d quantum %u ECN threshold %llu\n", cl->id, cl->quantum, ecn_thresh_bytes);
			}
			else if (DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_TCN || DWRR_QDISC_ECN_SCHEME == DWRR_QDISC_CODEL)
			{
                /* Get enqueue time stamp */
                skb->tstamp = ktime_get();
            }
		}
		else
		{
			if (net_xmit_drop_count(ret))
			{
				qdisc_qstats_drop(sch);
				qdisc_qstats_drop(cl->qdisc);
			}
		}
		return ret;
	}
}

/* We don't need this */
static unsigned int dwrr_qdisc_drop(struct Qdisc *sch)
{
	return 0;
}

/* We don't need this */
static int dwrr_qdisc_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	return 0;
}

/* Release Qdisc resources */
static void dwrr_qdisc_destroy(struct Qdisc *sch)
{
	struct dwrr_sched_data *q = qdisc_priv(sch);
	int i;

	if (q->queues)
	{
		for (i = 0; i < DWRR_QDISC_MAX_QUEUES && (q->queues[i]).qdisc; i++)
			qdisc_destroy((q->queues[i]).qdisc);

		kfree(q->queues);
		q->queues=NULL;
	}
	qdisc_watchdog_cancel(&q->watchdog);
}

static const struct nla_policy dwrr_qdisc_policy[TCA_TBF_MAX + 1] = {
	[TCA_TBF_PARMS] = { .len = sizeof(struct tc_tbf_qopt) },
	[TCA_TBF_RTAB]	= { .type = NLA_BINARY, .len = TC_RTAB_SIZE },
	[TCA_TBF_PTAB]	= { .type = NLA_BINARY, .len = TC_RTAB_SIZE },
};

/* We only leverage TC netlink interface to configure rate */
static int dwrr_qdisc_change(struct Qdisc *sch, struct nlattr *opt)
{
	int err;
	struct dwrr_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_TBF_PTAB + 1];
	struct tc_tbf_qopt *qopt;
	__u32 rate;

	err = nla_parse_nested(tb, TCA_TBF_PTAB, opt, dwrr_qdisc_policy);
	if (err < 0)
		return err;

	err = -EINVAL;
	if (!(tb[TCA_TBF_PARMS]))
		goto done;

	qopt = nla_data(tb[TCA_TBF_PARMS]);
	rate = qopt->rate.rate;
	/* convert from bytes/s to b/s */
	q->rate.rate_bps = (u64)rate << 3;
	precompute_ratedata(&q->rate);
	err = 0;
	printk(KERN_INFO "sch_dwrr: rate %llu Mbps\n", q->rate.rate_bps/1000000);

 done:
	return err;
}

/* Initialize Qdisc */
static int dwrr_qdisc_init(struct Qdisc *sch, struct nlattr *opt)
{
	int i;
	struct dwrr_sched_data *q = qdisc_priv(sch);
	struct Qdisc *child;

	if (sch->parent != TC_H_ROOT)
		return -EOPNOTSUPP;

	q->queues = kcalloc(DWRR_QDISC_MAX_QUEUES, sizeof(struct dwrr_class), GFP_KERNEL);
	if (!(q->queues))
		return -ENOMEM;

	q->tokens = 0;
	q->time_ns = ktime_get_ns();
	q->last_idle_time_ns = ktime_get_ns();
	q->sum_len_bytes = 0;
	q->round_time_ns = 0;
	q->quantum_sum = 0;
	q->quantum_sum_estimate = 0;
	qdisc_watchdog_init(&q->watchdog, sch);
	INIT_LIST_HEAD(&(q->active_list));

	for (i = 0;i < DWRR_QDISC_MAX_QUEUES; i++)
	{
		/* bfifo is in bytes */
		child = fifo_create_dflt(sch, &bfifo_qdisc_ops, DWRR_QDISC_MAX_BUFFER_BYTES);
		if (child!=NULL)
			(q->queues[i]).qdisc = child;
		else
			goto err;

		/* Initialize per-queue variables */
		INIT_LIST_HEAD(&((q->queues[i]).alist));
		(q->queues[i]).id = i;
		(q->queues[i]).deficit = 0;
		(q->queues[i]).active = 0;
		(q->queues[i]).curr = 0;
		(q->queues[i]).len_bytes = 0;
		(q->queues[i]).start_time_ns = ktime_get_ns();
		(q->queues[i]).last_pkt_time_ns = ktime_get_ns();
		(q->queues[i]).last_pkt_len_ns = 0;
		(q->queues[i]).quantum = 0;
        (q->queues[i]).count = 0;
        (q->queues[i]).lastcount = 0;
        (q->queues[i]).marking = false;
        (q->queues[i]).rec_inv_sqrt = 0;
        (q->queues[i]).first_above_time = 0;
        (q->queues[i]).mark_next = false;
        (q->queues[i]).ldelay = 0;
	}
	return dwrr_qdisc_change(sch,opt);
err:
	dwrr_qdisc_destroy(sch);
	return -ENOMEM;
}

static struct Qdisc_ops dwrr_qdisc_ops __read_mostly = {
	.next = NULL,
	.cl_ops = NULL,
	.id = "tbf",
	.priv_size = sizeof(struct dwrr_sched_data),
	.init = dwrr_qdisc_init,
	.destroy = dwrr_qdisc_destroy,
	.enqueue = dwrr_qdisc_enqueue,
	.dequeue = dwrr_qdisc_dequeue,
	.peek = dwrr_qdisc_peek,
	.drop = dwrr_qdisc_drop,
	.change = dwrr_qdisc_change,
	.dump = dwrr_qdisc_dump,
	.owner = THIS_MODULE,
};

static int __init dwrr_qdisc_module_init(void)
{
	if (!dwrr_qdisc_params_init())
		return -1;

	printk(KERN_INFO "sch_dwrr: start working\n");
	return register_qdisc(&dwrr_qdisc_ops);
}

static void __exit dwrr_qdisc_module_exit(void)
{
	dwrr_qdisc_params_exit();
	unregister_qdisc(&dwrr_qdisc_ops);
	printk(KERN_INFO "sch_dwrr: stop working\n");
}

module_init(dwrr_qdisc_module_init);
module_exit(dwrr_qdisc_module_exit);
MODULE_LICENSE("GPL");
