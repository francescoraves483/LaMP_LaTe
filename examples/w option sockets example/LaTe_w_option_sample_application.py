#! /usr/bin/python3
from threading import Thread, Lock
from dataclasses import dataclass
import sys,getopt,os,time,socket

# Data class to store the options read from command line (i.e. IPv4 and port to bind the UDP and TCP sockets to)
@dataclass
class struct_options:
	IPv4: str = None
	port: int = 0

mutex_exitflag=Lock()
exitflag=0

# Thread function to manage the reception of the UDP packets, containing the per-packet data from LaTe
# This is also the function used to process the per-packet data, for each packet (in this example, we are 
# just printing each reported metric and its value)
def udp_rx_loop(udp_descriptor,accepted_id,late_fields):
	while True:
		data,addr=udp_descriptor.recvfrom(2048)

		# Convert bytes to string
		decoded_data=data.decode("utf-8").split(",")

		# This is just done to correctly terminate the loop (see the terminate_udp_rx_loop() function, starting from line 71)
		if decoded_data[0] == "terminate":
			if decoded_data[1] == accepted_id:
				print("UDP rx loop termination requested")
				break
			else:
				continue

		# Discard all the UDP packets which are not starting with 'LaTe' and the correct ID, as received in 'LaTeINIT' (point (a.2) in ./LaTe -h)
		# The correct ID is passed to this thread function by means of its 'accepted_id' argument
		if decoded_data[0] != "LaTe":
			print("Warning: data ignored. Expected a LaTe packet, but received",decoded_data[0])
			continue;

		if decoded_data[1] != accepted_id:
			print("Warning: data ignored. Expected test ID",accepted_id,"but received",decoded_data[1])
			continue;

		# When a UDP packet is received, parse/process the data contained inside using the fields received in 'LateINIT' (point (a.3) in ./LaTe -h)
		# The fields received in 'LaTeINIT' are passed to this thread function by means of its 'late_fields' argument
		# .... process here the data related to each packet ....
		print("UDP | received data:")
		for i in range(len(late_fields)):
			print("\t",late_fields[i],":",decoded_data[i+2])

		mutex_exitflag.acquire()
		if exitflag == 1:
			break
		mutex_exitflag.release()

	print("UDP rx loop terminated")

# Function that is called every time a 'LaTeEND' packet is received from LaTe, containing
# the final report data and used to process that data (in this example, we are just printing
# each reported metric and its value)
def final_test_data_operations(decoded_data):
	print("TCP | received data:")

	# .... process here the final report data ....
	for couple in decoded_data[2:]:
		field=couple.split("=")[0]
		value=couple.split("=")[1]
		print("\t",field,":",value)

# Function to properly terminate the UDP reception loop (udp_rx_loop) when a 'LaTeEND' packet
# is received, telling the application to stop waiting for new UDP packets with per-packet data
def terminate_udp_rx_loop(udp_descriptor,opts,lamp_session_id):
	# Terminate the udp_rx_loop thread if it is not blocked inside 'recv()'
	# 'exitflag' is protected with a mutex to avoid any concurrent write/read
	mutex_exitflag.acquire()
	exitflag=1
	mutex_exitflag.release()

	# Sending to ourselves a packet containing "terminate," and the current ID, allows us to exit from the 'recv()'' in the
	# "udp_rx_loop" thread in a "cleaner" way
	termination_str="terminate," + str(lamp_session_id)
	udp_descriptor.sendto(termination_str.encode(),(opts.IPv4,opts.port))

# ---------------------------------------------------------------------------------------------------
# ---- Main function, executed at the beginning and containing the main logic of the application ----
# ---------------------------------------------------------------------------------------------------
def main (argv):
	# Parse the command-line options
	opts=parse_options(argv)

	# Create a TCP socket as server (point (1) in ./LaTe -h)
	try:
		tcp_descriptor=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
	except IOError as e:
		print("Error when opening the TCP socket. Details:",e.strerror)
		sys.exit(1)

	tcp_descriptor.bind((opts.IPv4,opts.port))

	# We are accepting only one connection at a time
	# This is not mandatory: it was our choice in this example to accept one connection at a time
	# (In reality, 2 connections will be probably accepted, as the backlog parameter in listen() 
	# is not very rigid and it is more intended as an "hint" given to the kernel)
	tcp_descriptor.listen(1)

	# Create a UDP socket
	try:
		udp_descriptor=socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
	except IOError as e:
		print("Error when opening the UDP socket. Details:",e.strerror)
		sys.exit(1)

	udp_descriptor.bind((opts.IPv4,opts.port))

	# Main TCP server loop
	while True:
		# Wait for a LaTe instance (TCP client) to connect (point (1) in ./LaTe -h)
		print("Waiting for a connection to " + opts.IPv4 + ":" + str(opts.port) + "...")

		tcp_descriptor_client,address=tcp_descriptor.accept()

		# Reset 'exitflag' to 0
		# No mutex is used as there should be no 'udp_rx_loop' running at this point
		exitflag=0

		print("New connection from: ",address)

		# Wait for the first TCP packet
		data=tcp_descriptor_client.recv(2048)

		# If the peer disconnects too early, print an error message and wait for another connection
		if len(data)==0:
			print("Error: LaTe was disconnected too early.")
			continue

		# Convert 'data' from bytes to string
		decoded_data=data.decode("utf-8").split(",")

		# Check if this packet is a 'LaTeINIT' one or if is it already a 'LaTeEND' packet (client-side unidirectional testing)
		if decoded_data[0] != "LaTeINIT" and decoded_data[0] != "LaTeEND":
			# No known packet has been received, close the socket and wait for a new connection
			print("Expected a LaTeINIT or LateEND packet, but received",decoded_data[0])
			tcp_descriptor_client.close()
			continue;
		elif decoded_data[0] == "LaTeEND" :
			# If 'LaTeEND' is received without any 'LaTeINIT' preceding it, a unidirectional client is involved and no per-packet
			# data is expected to be received. In this case, just save all the report data contained inside, processing it
			# depending on the user's needs (calling, in our case, final_test_data_operations()) (point (b.1) in ./LaTe -h)
			final_test_data_operations(decoded_data)
			tcp_descriptor_client.close()
			continue
		else:
			print("Received a",decoded_data[0],"packet")

		# If 'LateINIT' is received, save which per-packet fields will be sent via UDP and the current test ID (point (a.1) ./LaTe -h)
		# Save the current LaMP session ID
		lamp_session_id=decoded_data[1]

		late_fields=[]
		# Gather the field names from the first TCP packet (i.e. 'LaTeINIT')
		for field in decoded_data[2].split("=")[1].split(";"):
			late_fields.append(field)
		
		print("Current ID:",lamp_session_id)

		# Start waiting for and receiving UDP packets, in a separate thread (point (a.1) ./LaTe -h)
		# udp_rx_loop() will take into account the points (a.2) and (a.3) in ./LaTe -h
		t=Thread(target=udp_rx_loop,args=(udp_descriptor,lamp_session_id,late_fields))
		try:
			t.start()
		except:
			print("Error: unable to start UDP reception thread.")
			break

		# Wait for the last TCP packet
		data=tcp_descriptor_client.recv(2048)

		# If the TCP client disconnects before receiving the 'LaTeEND' packet, print an error and terminate the current connection
		if len(data)==0:
			print("Error: LaTe was disconnected before LaTeEND could be received.")
			terminate_udp_rx_loop(udp_descriptor,opts,lamp_session_id)
			t.join()
			tcp_descriptor_client.close()
			continue
		
		decoded_data=data.decode("utf-8").split(",")

		# Check if this packet is a 'LaTeEND' one
		if decoded_data[0] != "LaTeEND":
			print("Expected a LaTeEND packet, but received",decoded_data[0])
			print("No final report data can be obtained.")
		else:
			# When 'LateEND' is received and if it has the right ID, save all the report data contained inside, processing it
			# (point (a.4) in ./LaTe -h)
			print("Received a",decoded_data[0],"packet")

			if decoded_data[1] != lamp_session_id:
				print("Warning: data ignored. Expected test ID",lamp_session_id,"but received",decoded_data[1])
			else:
				# If 'LaTeEND' has the third field set to 'srvtermination', it should be considered just a way to gracefully
				# terminate the current connection after a unidirectional test, without carrying any particular final report data
				# In this case, we should not call the final_test_data_operations() function, as defined in line 60 (point (a.4) in ./LaTe -h)
				if decoded_data[2] != "srvtermination":
					# Decode the final test data
					final_test_data_operations(decoded_data)

		# Stop waiting for new UDP packets (point (a.4) in ./LaTe -h)
		terminate_udp_rx_loop(udp_descriptor,opts,lamp_session_id)
		t.join()

		# Close any TCP socket which was left open before accepting a new connection on the same port (point (2) in ./LaTe -h)
		tcp_descriptor_client.close()



# ---------------- Functions to manage the command line options (i.e. IP and port to bind the sockets to) --------------------------
# ----- This is just an example, using getopt: IP and port could be obtained by means of any other method or even hardcoded --------
# ----------------------------------------------------------------------------------------------------------------------------------
# This function is used to print the application usage and then exit with an error code equal to 'exit_value'
def print_short_info_and_exit(exit_value):
	print("Usage: python3 " + os.path.basename(__file__) + " -a <address to bind the socket to>:<port>")
	sys.exit(exit_value)

# Function used to parse the command line options, supporting -h (to print a short help) and -a (or --bindaddress - to specify <IP>:<port>)
def parse_options(argv):
	# Create a new 'struct_options' (see the @dataclass at the beginning of this file)
	options=struct_options()

	# Command line options parser
	try:
		opts,args=getopt.getopt(argv,"ha:e:",["bindaddress=",])
	except getopt.GetoptError as arg_error:
		print(str(arg_error))
		print_short_info_and_exit(1)

	for opt,arg in opts:
		if opt == '-h':
			print_short_info_and_exit(0);
		elif opt in ("-a", "--lateaddress"):
			# Parse the <IP>:<port> string
			split_arg=arg.split(":")
			options.IPv4=split_arg[0]
			options.port=int(split_arg[1])

			# Check if the specified IP and port are correct
			try:
				socket.inet_pton(socket.AF_INET,options.IPv4)
			except socket.error:
				print("Error. Invalid LaTe bind IPv4 address.")
				print_short_info_and_exit(1)

			if(options.port<=0 or options.port>65535):
				print("Error. Port" + options.port + "is invalid.")
				print_short_info_and_exit(1)
		else:
			print("Error. An unhandled option was encountered.")
			sys.exit(2)

	# Check if an IP and port were actually specified; if not, exit with an error
	# In this example, we made them mandatory, but they can also be initialized to some default values, for instance
	if(options.IPv4 == None):
		print("Error. No bind address has been specified.")
		print_short_info_and_exit(1)

	if(options.port == 0):
		print("Error. No port has been specified.")
		print_short_info_and_exit(1)

	return options;



# ---------------------------------------------------------------------------------------------------
if __name__ == "__main__":
	main(sys.argv[1:])