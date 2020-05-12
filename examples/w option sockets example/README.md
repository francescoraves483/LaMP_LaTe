**Python 3 example application receiving data through the LaTe -w option**

This Python 3 code represents an example of application which receives data through two parallel sockets (a TCP socket and a UDP socket) which are opened when the *-w* option, in LaTe (starting from version *0.1.6-beta, 20200511e*) is specified.

This simple example, even though we tried to made it as complete as possibile, including some error handling, limits itself to printing the received information, but it can be used as a base to build more complex applications, using Python but also other programming languages.

Of course, the logic inside the *.py* file is not intended to be rigid, and it can be adapted depending on your needs, as long as the packets coming from LaTe, when *-w* is specified, are properly handled.

For your conveniency, we report here also the LaTe help section related to the *-w* option (which can be read directly inside the program, by launching it with the *-h* option:
```
-w <IPv4:port>: output per-packet and report data, just like -W and -f for a CSV file, to a socket, which can then be
	read by any other application for further processing. To improve usability, the data is sent towards the selected IPv4
	and port in a textual, CSV-like human-readable format.

	When no port is specified, "STRINGIFY(DEFAULT_W_SOCKET_PORT)" will be used.

	After <IPv4:port>, it is possibile to specify an interface, through its name, to bind the socket to, for instance:
	'-w 192.168.1.101:46001,enp2s0'; if no interface is specified, the socket will be bound to all interfaces.

	This options involves the usage of two parallel sockets, bounded and connected to the same port (and, optionally,
	interface): one UDP socket, for sending the per-packer data, and one TCP socket, for sending more sentitive data,
	namely an initial packet telling how to interpret the comma separated fields sent via the UDP socket, for the
	per-packet data, and a final packet containing the final report data (including min, max, average, and so on).

	The initial packet is formatted in this way: 'LaTeINIT,<LaMP ID>,fields=f1;f2;...;fn', where <LaMP ID> is the
	current test ID, and 'fields=' is telling the meaning of the comma-separated fields which will be sent via UDP,
	e.g. 'LaTeINIT,55321,fields=seq;latency;tx_timestamp;error'.

	Each UDP packet will be instead formatted in this way: 'LaTe,<LaMP ID>,<f1>,<f2>,...,<fn>', where <fi> is the value
	of the i-th field, as described in the first TCP packet, e.g. 'LaTe,55321,2,0.388,1588953642.084513,0'.

	Then, at the end of a test, a final packet will be sent via TCP, formatted as: 'LaTeEND,<LaMP ID>,f1=<f1>,...,fn=<fn>'
	where fi=<fi> are fields containing the final report data, e.g. timestamp=1588953727,clientmode=pinglike,..., reported
	in a very similar way to what it is normally saved inside an -f CSV file.

	An application reading this data should:
		1) Create a TCP socket, as server, waiting for a LaTe instance (TCP client) to connect. There should be then two cases:
		a.1) If 'LateINIT' is received, save which per-packet fields will be sent via UDP and the current test ID and start
			 waiting for and receiving UDP packets, after opening a UDP socket.
		a.2) Discard all the UDP packets which are not starting with 'LaTe' and the correct ID, saved in (a.1).
		a.3) When a UDP packet is received, parse/process the data contained inside using the fields received in (a.1).
		a.4) When 'LateEND' is received and if it has the right ID, stop waiting for new UDP packets and save all the report data
			 contained inside, processing it depending on the user needs. If 'LaTeEND' has the third field set to 'srvtermination',
			 it should be considered just a way to gracefully terminate the current connection after a unidirectional test, without
			 carrying any particular final report data.
		b.1) If 'LaTeEND' is received without any 'LaTeINIT' preceding it, a unidirectional client is involved and no per-packet
			 data is expected to be received. In this case, just save all the report data contained inside, processing it
			 depending on the user's needs.\n" \
		2) Close any TCP socket which was left open before accepting a new connection on the same port."

	When a client in unidirectional mode is involved, no per-packet data or 'LaTeINIT' is sent with -w, as they are managed by
	the server. A 'LaTeEND' packet, with the final statistics, will be sent via TCP at the end of the test only.

	This options applies to a server only in unidirectional mode; in this case, 'LaTeEND' won't contain any final report, but it 
	will just be formatted as 'LaTe,<LaMP ID>,srvtermination' and it can be used to gracefully terminate the connection from the 
	application reading the -w data. A server, during a unidirectional test, can only send per-packet data to an external application 
	via TCP/UDP.
```

In order to launch this example, you need `python3`. You can then execute it by using:
```
python3 LaTe_w_option_sample_application.py -a <IP>:<port>
```
For instance, to receive packets over the loopback interface on port 46001:
```
python3 LaTe_w_option_sample_application.py -a 127.0.0.1:46010
```