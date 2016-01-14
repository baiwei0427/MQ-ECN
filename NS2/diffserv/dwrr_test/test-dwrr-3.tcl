set ns [new Simulator]

set flows 8
set K 83;	#The per-port ECN marking threshold
set marking_scheme 0

set RTT 0.0001
set DCTCP_g_ 0.0625
set ackRatio 1
set packetSize 1460
set lineRate 10Gb

set simulationTime 0.006

Agent/TCP set windowInit_ 10
Agent/TCP set ecn_ 1
Agent/TCP set old_ecn_ 1
Agent/TCP set dctcp_ false
Agent/TCP set dctcp_g_ $DCTCP_g_
Agent/TCP set packetSize_ $packetSize
Agent/TCP set window_ 1256
Agent/TCP set slow_start_restart_ false
Agent/TCP set minrto_ 0.01 ; # minRTO = 10ms
Agent/TCP set windowOption_ 0
Agent/TCP/FullTcp set segsize_ $packetSize
Agent/TCP/FullTcp set segsperack_ $ackRatio;
Agent/TCP/FullTcp set spa_thresh_ 3000;
Agent/TCP/FullTcp set interval_ 0.04 ; #delayed ACK interval = 40ms

Queue set limit_ 1000

Queue/DWRR set queue_num_ 1
Queue/DWRR set mean_pktsize_ [expr $packetSize + 40]
Queue/DWRR set port_thresh_ $K
Queue/DWRR set marking_scheme_ $marking_scheme
Queue/DWRR set estimate_round_alpha_ 0.75
Queue/DWRR set estimate_quantum_alpha_ 0.75
Queue/DWRR set estimate_round_idle_interval_bytes_ 1500
Queue/DWRR set estimate_quantum_interval_bytes_ 1500
Queue/DWRR set estimate_quantum_enable_timer_ false
Queue/DWRR set dq_thresh_ 10000
Queue/DWRR set estimate_rate_alpha_ 0.875
Queue/DWRR set link_capacity_ $lineRate
Queue/DWRR set deque_marking_ false
Queue/DWRR set debug_ true

Queue/RED set bytes_ false
Queue/RED set queue_in_bytes_ true
Queue/RED set mean_pktsize_ [expr $packetSize + 40]
Queue/RED set setbit_ true
Queue/RED set gentle_ false
Queue/RED set q_weight_ 1.0
Queue/RED set mark_p_ 1.0
Queue/RED set thresh_ $K
Queue/RED set maxthresh_ $K

set mytracefile [open mytracefile.tr w]
$ns trace-all $mytracefile
set throughputfile [open throughputfile.tr w]
set tot_qlenfile [open tot_qlenfile.tr w]

proc finish {} {
	global ns mytracefile throughputfile tot_qlenfile
	$ns flush-trace
	close $mytracefile
	close $throughputfile
	close $tot_qlenfile
	exit 0
}

set switch [$ns node]
set receiver [$ns node]

$ns simplex-link $switch $receiver $lineRate [expr $RTT/4] DWRR
$ns simplex-link $receiver $switch $lineRate [expr $RTT/4] DropTail

set L [$ns link $switch $receiver]
set q [$L set queue_]
$q set-thresh 0 $K
$q attach-total $tot_qlenfile

#Service type 1 senders
for {set i 0} {$i < $flows} {incr i} {
	set n1($i) [$ns node]
    $ns duplex-link $n1($i) $switch $lineRate [expr $RTT/4] RED
	set tcp1($i) [new Agent/TCP/FullTcp/Sack]
	set sink1($i) [new Agent/TCP/FullTcp/Sack]
	$tcp1($i) set serviceid_ 0
	$sink1($i) listen

	$ns attach-agent $n1($i) $tcp1($i)
    $ns attach-agent $receiver $sink1($i)
	$ns connect $tcp1($i) $sink1($i)

	set ftp1($i) [new Application/FTP]
	$ftp1($i) attach-agent $tcp1($i)
	$ftp1($i) set type_ FTP
	$ns at [expr 0.0] "$ftp1($i) start"
}

$ns at [expr $simulationTime] "finish"
$ns run
