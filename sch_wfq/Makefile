obj-m += sch_wfq.o
sch_wfq-y := main.o params.o

all:
	make -C /lib/modules/3.18.11-dctcp-measurement/build M=$(PWD) modules
clean:
	make -C /lib/modules/3.18.11-dctcp-measurement/build M=$(PWD) clean
