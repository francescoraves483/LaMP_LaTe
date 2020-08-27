
<img src="./Logo/LaTe_LaMP_logo.png" align="center">

# Main information

## What is LaTe?

**LaTe** is a flexible, client-server, multi-protocol* **La**tency **Te**ster, based on the custom **LaMP** protocol (**La**tency **M**easurement **P**rotocol) and running on **Linux** - _version 0.1.6-beta_.

This repository is the main one for what concerns both **LaTe** and the **LaMP** custom protocol, including its specifications.

As we are still in a beta stage, we highly welcome any contribution, improvement, bug report or suggestion, both to **LaTe** and to the **LaMP** specifications.
Other than using GitHub, you can freely write to <francescorav.es483@gmail.com>: we will try to reply you as soon as possible. Thank you!

\* In the current version, only **LaMP** over **IPv4** and **UDP** is supported, but we plan to implement other protocols in the future. **LaMP** over **AMQP 1.0**, via an additional Qpid Proton module, is also supported when LaTe is compiled with `compilePCfull`.

## Makefile

A simple **Makefile** is provided. This file can be used to compile LaTe in the Linux system in which this repository was cloned, with `gcc`. Any improvement to this file is highly welcome.

Additional targets are also defined; in particular:
- `compilePCdebug`, to compile for the current platform, with `gcc` and the flag `-g` to generate debug informations, to be used with `gdb`.
- `compilePCfull`, to compile an "extended" version of LaTe supporting also the **AMQP 1.0** protocol (option `-a`, or `--amqp-1.0`), thanks to the Qpid Proton library. In order to use this target and compile LaTe with the AMQP 1.0 support, you need to install Qpid Proton C first (it can be downloaded [here](https://qpid.apache.org/releases/qpid-proton-0.31.0/). To install it, after extracting the `.tar.gz` archive, you can refer to the instructions contained in the `INSTALL.md` file. Tested with versions [0.30.0](https://qpid.apache.org/releases/qpid-proton-0.30.0/) and 0.31.0.). **If you don't need to test over AMQP 1.0, it is highly suggested to compile the normal version of LaTe, supporting, for the time being, only UDP.**
- `compilePCfulldebug`, as `compilePCfull`, but adding the `-g` flag to generate debug informations, to be used with `gdb`.
- `compileAPU`, as we also used **LaTe** to perform wireless latency measurements on [PC Engines APU1D embedded boards](https://pcengines.ch/apu1d.htm), running [OpenWrt](https://github.com/francescoraves483/OpenWrt-V2X), we defined an additional target to cross-compile LaTe for the boards. This command should work when targeting any **x86_64** embedded board running **OpenWrt**, after the toolchain has been properly set up (tested with OpenWrt 18.06.1). If you want to cross-compile LaTe for other Linux-based platforms, you will need to change the value of **CC_EMBEDDED** inside the Makefile with the compiler you need to use.
- `compileAPUdebug`, as before, but with the `-g` flag to generate debug informations for `gdb`.

The additional **Makefile_termux** makefile can be used when building the LaTe version supporting AMQP on a non-rooted **Android** device, using Termux.
When installing Qpid Proton, you can choose to install it inside `/data/data/com.termux/files/home/libs`, which will make root privileges unnecessary when running `make install`.
This makefile, when running `make -f Makefile_termux compilePCfull` will look for Qpid Proton inside `/data/data/com.termux/files/home/libs`, allowing you to compile also the full version of LaTe.
In this particular case, before launching LaTe, after compiling it, remember to add the new library path to `LD_LIBRARY_PATH`:
```
export LD_LIBRARY_PATH=/data/data/com.termux/files/home/libs
```

Please note that in order to compile Qpid Proton on Android, you may need to manually patch the library. For your conveniency, an already patched QPid Proton C library (version 0.30.0) is available [here](https://github.com/francescoraves483/qpid-proton).

**LaTe** has been extensively tested on Linux kernel versions 4.14, 4.15, 5.0 and 5.4 and it is currently using the [**Rawsock library, version 0.3.4**](https://github.com/francescoraves483/Rawsock_lib).

## How to compile

Clone the master branch of the repository (`--recursive` is needed in order to clone the Rawsock library submodule too):
```
git clone --recursive https://github.com/francescoraves483/LaMP_LaTe.git
cd LaMP_LaTe
```
Compile:
```
make
```

The executable is called `LaTe`.


# LaMP specifications

The full LaMP specifications can be found here: [LaMP specifications, revision 2.0](./LaMP/LaMP_specifications_rev2.0.pdf).

# LaTe user guide (v0.1.6-beta+)

For the latest options and their description, please refer to the help available when calling `LaTe --help`, as it is updated every time a new option is introduced and/or modified.

## Modes

LaTe supports different modes, based on the **client-server** paradigm, which is the one supported by LaMP.
The first command line argument should correspond to the desired mode (both short options and equivalent long options are available for each mode):

- **\-c \<destination address\>** \| **\-\-client \<destination address\>**: client mode
- **\-s** \| **\-\-server**: server mode
- **\-l** \| **\-\-loopback-client**: client mode, binding to a loopback interface
- **\-m** \| **\-\-loopback-server**: server mode, binding to a loopback interface
  
Alternatively, it is possible to specify:
- **\-h** \| **\-\-help**: to print the long version of the help, directly within LaTe, including the available interfaces and their internal indeces, to be passed to LaTe using _-I_.
- **\-v** \| **\-\-version**: to print additional version information

*-v* or *-h* should be used alone, without any additional parameter.

When *-c* (or *\-\-client*) is selected, the destination address must be specified, depending on the protocol. **In UDP mode, this address corresponds to the destination IPv4 address.**

When a client (either *-c* or *-l*) is selected, just after *-c/-l*, a mode has to be specified:
- **\-B** (or **\-\-bidir**) for the standard ping-like bidirectional mode, working like ICMP Echo Request/Reply, but in a client-server fashion (read also the [LaMP specifications](./LaMP/LaMP_specifications_rev2.0.pdf)). This mode allows the user to test the RTT.
- **\-U** (or **\-\-unidir**) for a unidirectional testing mode, in which the client sends a LaMP packet to the server, and the latter tries to compute a unidirectional latency, based on its own timestamp and on the one embedded inside the packet by the client, without the need of generating a reply. It works only if there is an external way of keeping the devices’ clocks perfectly synchronized, with, if possible, sub-ms precision. One way of synchronizing the clocks of the devices running LaTe is relying on the Precision Time Protocol (*PTP* - for instance with tools like `ptp4l`/`phc2sys` or `ptpd`).

## Protocols

A protocol should be then specified. As of now, LaTe supports latency measurements only with UDP, over IPv4, but other protocols will be implemented in the future.
If an "extended" version of LaTe is compiled with `compilePCfull` or `compilePCfulldebug` and the Qpid Proton AMQP 1.0 library is installed, also latency measurements with AMQP 1.0 and a compatible message broker are possible.

Protocols are specified through specific arguments. Supported protocols are:
- **\-u** \| **\-\-udp**: UDP (LaMP over UDP)
- **\-a** \| **\-\-amqp-1.0**: AMQP 1.0 (LaMP over AMQP 1.0 - only when `compilePCfull`/`compilePCfulldebug` is used to compile LaTe and Qpid Proton is installed)


## Client and server specific options

Then, client and sever specific options should be specified.

You can find a complete description of these options after compiling LaTe and calling it with the **\-h** (or **\-\-help**) option.

Each option is described, inside LaTe, in this format:
```
<long option>
<short option> <argument, if necessary>: <description>
```
For instance, this is the description for the `-n` option:
```
--num-packets
-n <total number of packets to be sent>: specifies how many packets to send (default: 600).
```

## Example of usage

Some of these examples of usage are also displayed when calling the program with the *-h* option.

### Non-RAW sockets and UDP:
*Client (port 7000, ping-like, 100 packets, one packet every 100 ms, LaMP payload size: 700 B):*

`LaTe -c 192.168.1.180 -p 7000 -B -u -t 100 -n 100 -P 700`

*Server (port 7000, timeout: 5000 ms):*

`LaTe -s -p 7000 -t 5000 -u`

### Non-RAW sockets and UDP, using long options:
*Client (port 7000, ping-like, 100 packets, one packet every 100 ms, LaMP payload size: 700 B):*

`LaTe --client 192.168.1.180 --port 7000 --bidir --udp --interval 100 --num-packets 100 --payload-bytes 700`

*Server (port 7000, timeout: 5000 ms):*

`LaTe --server --port 7000 --server-timeout 5000 --udp`

### RAW sockets and UDP:
*Client (port 7000, ping-like, 100 packets, one packet every 100 ms, LaMP payload size: 700 B):*

`LaTe -c 192.168.1.180 -p 7000 -B -u -t 100 -n 100 -M D8:61:62:04:9C:A2 -P 700 -r`

*Server (port 7000, timeout: 5000 ms):*

`LaTe -s -p 7000 -t 5000 -u -r`

### Non-RAW sockets and UDP, over loopback, with default options:
*Client (port 46000, ping-like, 600 packets, one packet every 100 ms, LaMP payload size: 0 B, user-to-user):*

`LaTe -l -B -u`

*Server (port 46000, timeout: 4000 ms):*

`LaTe -m -u`

### Non-RAW sockets and UDP, over loopback, with default options, using long options:
*Client (port 46000, ping-like, 600 packets, one packet every 100 ms, LaMP payload size: 0 B, user-to-user):*

`./LaTe --loopback-client --bidir --udp`

*Server (port 46000, timeout: 4000 ms):*

`./LaTe --loopback-server --udp`



## Important notes

Few important notes about **LaTe** are listed below:

- In order to use raw sockets, root privileges are needed.
- Time intervals less or equal to 0 ms are not supported and will generate an error (i.e., after *-t*, or *\-\-interval*, a number *>= 1* should be specified), as they would make no sense in this context.
- When in UDP mode, LaMP payloads are supported up to 1448 B, in order not to exceed the Ethernet MTU, which would case fragmentation in non-raw mode and a transmission error in raw mode.
- When raw sockets are used (i.e. *-r* or *\-\-raw* is specified), a destination MAC address should be specified with *-M* (or *\-\-mac*).
- Specifying a port is required when working with LaMP over UDP (i.e. when using the *-u* option). If no port is specified, the program will use `46000` as default value.
- The server will adapt its mode (bidirectional or unidirectional) depending on the packets it receives from the client. Therefore, specifying *-B* or *-U* for a server will have no effect.
- When in non-raw mode, the destination MAC address is not required. If specified with *-M*, however, it won’t generate an error, but it will be simply ignored by LaTe.
- Even though it is possible to choose which confidence intervals to display, through option *-C* (or *\-\-confidence*), a CSV file will always contain all of them (.90, .95 and .99).
- There is currently no support yet for software kernel transmit and receive timestamps and hardware timestamps when in raw or unidirectional mode.
- The hardware timestamp feature is still partly experimental: it should work properly, and several tests have been performed on supported NICs, but more testing is still required and ongoing.

## Latency types

Four latency types are supported as of now. They are all computed thanks to seconds and microseconds timestamps.

In order to compute the delay, LaTe compares transmit and receive timestamps (managed respectively by who sends each test packet and who receives it).

**User-to-user (‘u’)**: in which the transmit timestamp (i.e. the client timestamp) is placed in the packet just before passing it to the send system call (for instance, just before passing the LaMP packet to a send call over a *SOCK_DRAM* socket when in UDP mode, or before passing the whole raw packet, containing IPv4, UDP and LaMP, to a send call related to an *ETH_P_ALL* raw socket); the receive timestamp is instead obtained from the real-time clock as soon as the packet has been received and it has been parsed, checking if it is of interest (i.e. if it is really LaMP, if it is has the correct ID and if it is actually the expected one). This type is the default one and it allows the user to measure an actual end-to-end ("application-to-application") latency.

**KRT - Kernel Receive Timestamp (‘r’)**: this mode is the one used by iputils ping (if this is wrong, please correct us) to compute the latency with ICMP Echo packets, at least in a standard case. The transmit timestamp is obtained like in the User-to-user mode, but the receive one is obtained directly from the Linux kernel, as every packet is received. These timestamps are still software timestamps, and, according to the kernel documentation, they should be “generated just after a device driver hands a packet to the kernel receive stack”. This allows to reduce the application latency contribution from the receiver side when measuring the delay.

**Software transmit and receive timestamps ('s')**: this mode is available only if supported by the NIC and by the driver. Both the transmit (sender) and the receive (receiver) timestamps are obtained from the Linux kernel, as every request is sent and every reply is received. This feature relies on ancillary data and [CMSG](http://man7.org/linux/man-pages/man3/cmsg.3.html). *This latency type, as of now, is supported only for RTT measurements (i.e. in -B mode) and when non-raw sockets are used (i.e. no -r is specified).*

Example of supported NICs: Intel I219-V, Intel i210AT.

**Hardware transmit and receive timestamps ('h')**: this mode is available only if supported by the NIC and by the driver. Both the transmit and the receive timestamps (which are then compared to compute the latency) are obtained from the network adapter, as every request is sent and every reply is received, so, no kernel time should be considered. This feature relies on ancillary data and [CMSG](http://man7.org/linux/man-pages/man3/cmsg.3.html). *This latency type, as of now, is supported only for RTT measurements (i.e. in -B mode) and when non-raw sockets are used (i.e. no -r is specified).*

Example of supported NICs: Intel I219-V, Intel i210AT. Although it could be very useful, we were not able to find any USB-to-Ethernet adapter supporting hardware timestamps (as, normally, they use ASIX or Realtek chipsets).


## How does the client/server work?

When the client has been launched, it will try to transmit initialization packets to the specified server. 

If no server is found, the client will stop sending these packets after a certain number of attempts and will terminate its execution.

If a server has been found, the client will start sending LaMP packets to the server and the measurement session will start.

In **bidirectional** mode, the server will try to reply at each client request, and the client will look at the replies to compute the statistics returned by the program.

In **unidirectional mode**, the server will try to compute the latency from the timestamp embedded by the client inside each LaMP packet, sending back to the client the report data as the session finishes, for visualization.

At the end of the session, the average, minimum and maximum latency values will be reported, together with other metrics, including the number of lost packets and an indicative measure of the out-of-order count, as the number of times a decreasing sequence number is detected in the received packets (i.e. in the received replies). If *-f* (or *\-\-report-file*) is specified, this data will also be written to a *.csv* file.

The same metrics can also be sent to [Graphite](https://graphiteapp.org/), by using on the *-g* option (or its equivalent version: *\-\-report-graphite*). More details on this option are available when calling LaTe with **\-\-help**.

Both client and server have a timeout set on the sockets they use to receive and send data. After few seconds of inactivity, they will declare the connection as terminated and terminate their execution (or, in case a daemon mode server is launched, a new session will start).

Both the client and the server follow the [LaMP specifications](./LaMP/LaMP_specifications_rev2.0.pdf)


## How does the server daemon mode work?

When *-d* (or *\-\-daemon*) is specified on a server, a special daemon mode will be activated.

When using this mode, the server will not terminate its execution after a session is correctly (or badly, due to an error) terminated, but it will, instead, start again listening for a client willing to connect. This allows to launch a single server for multiple successive client connections and measurement sessions.

In order to correctly terminate the server, the user can send the SIGUSR1 signal to the LaTe process on which the server is running, using `kill -s USR1 <pid>`.

The `<pid>` is an integer number univocally identifying each process (i.e. each “running program”)  running on the system.

The `<pid>` of the LaTe server program can be found by means of the `ps` utility (if it is not listed, use `ps -A`).

After giving the termination command, the current session will run until it will finish, then the program will be terminated as a normal, non-daemon, server.

This mechanism may be suject to changes in the future.

# Version information

The current non-development version is **0.1.6-beta**, using **LaMP rev 2.0** and **Rawsock library v0.3.4**.

Developed in **Politecnico di Torino**, licensed under **GPLv2**.

This is **open source**: you should never be charged for using or downloading LaTe and the source code should always be made available to you.
