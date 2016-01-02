#ifndef __PARAMS_H__
#define __PARAMS_H__

#include <linux/types.h>

/* Our module has at most 8 queues */
#define WFQ_QDISC_MAX_QUEUES 8
/* MTU(1500B)+Ethernet header(14B)+Frame check sequence (4B)+Frame check sequence(8B)+Interpacket gap(12B) */
#define WFQ_QDISC_MTU_BYTES 1538
/* Ethernet packets with less than the minimum 64 bytes (header (14B) + user data + FCS (4B)) are padded to 64 bytes. */
#define WFQ_QDISC_MIN_PKT_BYTES 64
/* Maximum (per queue/per port shared) buffer size (2MB)*/
#define WFQ_QDISC_MAX_BUFFER_BYTES 2000000

/* Debug mode is off */
#define	WFQ_QDISC_DEBUG_OFF 0
/* Debug mode is on */
#define	WFQ_QDISC_DEBUG_ON 1

/* Per port shared buffer management policy */
#define	WFQ_QDISC_SHARED_BUFFER 0
/* Per port static buffer management policy */
#define	WFQ_QDISC_STATIC_BUFFER 1

/* Disable ECN marking */
#define	WFQ_QDISC_DISABLE_ECN 0
/* Per queue ECN marking */
#define	WFQ_QDISC_QUEUE_ECN 1
/* Per port ECN marking */
#define WFQ_QDISC_PORT_ECN 2
/* Dequeue latency-based ECN marking. This is a general ECN marking approach for any packet scheduler */
#define WFQ_QDISC_DEQUE_ECN 5


/* Debug mode or not */
extern int WFQ_QDISC_DEBUG_MODE;
/* Buffer management mode: shared (0) or static (1)*/
extern int WFQ_QDISC_BUFFER_MODE;
/* Per port shared buffer (bytes) */
extern int WFQ_QDISC_SHARED_BUFFER_BYTES;
/* Bucket size in nanosecond*/
extern int WFQ_QDISC_BUCKET_NS;
/* Per port ECN marking threshold (bytes) */
extern int WFQ_QDISC_PORT_THRESH_BYTES;
/* ECN marking scheme */
extern int WFQ_QDISC_ECN_SCHEME;

/* Per queue ECN marking threshold (bytes) */
extern int WFQ_QDISC_QUEUE_THRESH_BYTES[WFQ_QDISC_MAX_QUEUES];
/* DSCP value for different queues */
extern int WFQ_QDISC_QUEUE_DSCP[WFQ_QDISC_MAX_QUEUES];
/* Weights for different queues*/
extern int WFQ_QDISC_QUEUE_WEIGHT[WFQ_QDISC_MAX_QUEUES];
/* Per queue static reserved buffer (bytes) */
extern int WFQ_QDISC_QUEUE_BUFFER_BYTES[WFQ_QDISC_MAX_QUEUES];

struct WFQ_QDISC_Param
{
	char name[64];
	int *ptr;
};

extern struct WFQ_QDISC_Param WFQ_QDISC_Params[6 + 4 * WFQ_QDISC_MAX_QUEUES + 1];

/* Intialize parameters and register sysctl */
int wfq_qdisc_params_init(void);
/* Unregister sysctl */
void wfq_qdisc_params_exit(void);

#endif
