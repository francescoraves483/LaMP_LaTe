CC_EMBEDDED=x86_64-openwrt-linux-musl-gcc
CC=gcc
CFLAGS=-I ./Rawsock_lib/ -Wall -O2
DEPS=common_thread.h options.h packet_structs.h report_manager.h timeval_subtract.h udp_client_raw.h udp_client.h udp_server_raw.h udp_server.h version.h common_socket_man.h timer_man.h common_udp.h
RAWSOCK_FILES=Rawsock_lib/rawsock.h Rawsock_lib/rawsock.c Rawsock_lib/ipcsum_alth.h Rawsock_lib/ipcsum_alth.c Rawsock_lib/rawsock_lamp.h Rawsock_lib/rawsock_lamp.c Rawsock_lib/minirighi_udp_checksum.h Rawsock_lib/minirighi_udp_checksum.c
CFILES=LatencyTester.c options.c report_manager.c common_socket_man.c udp_client_raw.c udp_client.c udp_server_raw.c udp_server.c timer_man.c common_udp.c common_thread.c

compilePC: LatencyTester.c options.c report_manager.c udp_client.c udp_server.c
	$(CC) -o LaTe $(DEPS) $(RAWSOCK_FILES) $(CFILES) -lpthread 

compilePCdebug: LatencyTester.c options.c report_manager.c udp_client.c udp_server.c
	$(CC) -o LaTe $(DEPS) $(RAWSOCK_FILES) $(CFILES) -g -lpthread 
	
compileAPU: LatencyTester.c options.c report_manager.c udp_client.c udp_server.c
	$(CC_EMBEDDED) -o LaTe $(DEPS) $(RAWSOCK_FILES) $(CFILES)

compileAPUdebug: LatencyTester.c options.c report_manager.c udp_client.c udp_server.c
	$(CC_EMBEDDED) -o LaTe $(DEPS) $(RAWSOCK_FILES) $(CFILES) -g

clean:
	rm LaTe