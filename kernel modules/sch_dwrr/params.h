#ifndef __PARAMS_H__
#define __PARAMS_H__

#include <linux/types.h>

/* CoDel uses a 1024 nsec clock, encoded in u32
 * This gives a range of 2199 seconds, because of signed compares
 */
typedef u32 codel_time_t;
typedef s32 codel_tdiff_t;
#define CODEL_SHIFT 10

/* Dealing with timer wrapping, according to RFC 1982, as desc in wikipedia:
 *  https://en.wikipedia.org/wiki/Serial_number_arithmetic#General_Solution
 * codel_time_after(a,b) returns true if the time a is after time b.
 */
#define codel_time_after(a, b)						\
	(typecheck(codel_time_t, a) &&					\
	typecheck(codel_time_t, b) &&					\
	((s32)((a) - (b)) > 0))
#define codel_time_before(a, b) 	codel_time_after(b, a)

#define codel_time_after_eq(a, b)					\
	(typecheck(codel_time_t, a) &&					\
	typecheck(codel_time_t, b) &&					\
	((s32)((a) - (b)) >= 0))
#define codel_time_before_eq(a, b)	codel_time_after_eq(b, a)

/* Our module has at most 8 queues */
#define DWRR_QDISC_MAX_QUEUES 8
/* MTU(1500B)+Ethernet header(14B)+Frame check sequence (4B)+Frame check sequence(8B)+Interpacket gap(12B) */
#define DWRR_QDISC_MTU_BYTES 1538
/* Ethernet packets with less than the minimum 64 bytes (header (14B) + user data + FCS (4B)) are padded to 64 bytes. */
#define DWRR_QDISC_MIN_PKT_BYTES 64
/* Maximum (per queue/per port shared) buffer size (2MB)*/
#define DWRR_QDISC_MAX_BUFFER_BYTES 2000000

/* Debug mode is off */
#define	DWRR_QDISC_DEBUG_OFF 0
/* Debug mode is on */
#define	DWRR_QDISC_DEBUG_ON 1

/* Per port shared buffer management policy */
#define	DWRR_QDISC_SHARED_BUFFER 0
/* Per port static buffer management policy */
#define	DWRR_QDISC_STATIC_BUFFER 1

/* Disable ECN marking */
#define	DWRR_QDISC_DISABLE_ECN 0
/* Per queue ECN marking */
#define	DWRR_QDISC_QUEUE_ECN 1
/* Per port ECN marking */
#define DWRR_QDISC_PORT_ECN 2
/* MQ-ECN for any packet scheduling algorithm */
#define DWRR_QDISC_MQ_ECN_GENER 3
/* MQ-ECN for round-robin packet scheduling algorithms */
#define DWRR_QDISC_MQ_ECN_RR 4
/* TCN */
#define DWRR_QDISC_TCN 5
/* CoDel ECN marking */
#define DWRR_QDISC_CODEL 6

#define DWRR_QDISC_MAX_ITERATION 10

/* The number of global (rather than 'per-queue') parameters */
#define DWRR_QDISC_NUM_GLOBAL_PARAMS 12

/* Global parameters */
/* Debug mode or not */
extern int DWRR_QDISC_DEBUG_MODE;
/* Buffer management mode: shared (0) or static (1)*/
extern int DWRR_QDISC_BUFFER_MODE;
/* Per port shared buffer (bytes) */
extern int DWRR_QDISC_SHARED_BUFFER_BYTES;
/* Bucket size in bytes */
extern int DWRR_QDISC_BUCKET_BYTES;
/* Per port ECN marking threshold (bytes) */
extern int DWRR_QDISC_PORT_THRESH_BYTES;
/* ECN marking scheme */
extern int DWRR_QDISC_ECN_SCHEME;
/* Alpha for quantum sum estimation */
extern int DWRR_QDISC_QUANTUM_ALPHA;
/* Alpha for round time estimation */
extern int DWRR_QDISC_ROUND_ALPHA;
/* Idle time interval */
extern int DWRR_QDISC_IDLE_INTERVAL_NS;
/* TCN threshold (1024 nanoseconds) */
extern int DWRR_QDISC_TCN_THRESH;
/* CoDel target (1024 nanoseconds) */
extern int DWRR_QDISC_CODEL_TARGET;
/* CoDel interval (1024 nanoseconds) */
extern int DWRR_QDISC_CODEL_INTERVAL;


/* Per queue ECN marking threshold (bytes) */
extern int DWRR_QDISC_QUEUE_THRESH_BYTES[DWRR_QDISC_MAX_QUEUES];
/* DSCP value for different queues */
extern int DWRR_QDISC_QUEUE_DSCP[DWRR_QDISC_MAX_QUEUES];
/* Quantum for different queues*/
extern int DWRR_QDISC_QUEUE_QUANTUM[DWRR_QDISC_MAX_QUEUES];
/* Per queue static reserved buffer (bytes) */
extern int DWRR_QDISC_QUEUE_BUFFER_BYTES[DWRR_QDISC_MAX_QUEUES];

struct dwrr_qdisc_param
{
	char name[64];
	int *ptr;
};

extern struct dwrr_qdisc_param DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + 4 * DWRR_QDISC_MAX_QUEUES + 1];

/* Intialize parameters and register sysctl */
bool dwrr_qdisc_params_init(void);
/* Unregister sysctl */
void dwrr_qdisc_params_exit(void);

#endif
