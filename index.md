
<img src="./Logo/LaTe_LaMP_logo.png" align="center">

# Main information

## What is LaTe?

**LaTe** is a flexible, client-server, multi-protocol* **La**tency **Te**ster, based on the custom **LaMP** protocol (**La**tency **M**easurement **P**rotocol) and running on **Linux** - _version 0.1.1-beta_.

This repository is the main one for what concerns both **LaTe** and the **LaMP** custom protocol, including its specifications.

As we are still in a beta stage, we highly welcome any contribution, improvement, bug report or suggestion, both to **LaTe** and to the **LaMP** specifications.
Other than using GitHub, you can freely write to <francescorav.es483@gmail.com>: we will try to reply you as soon as possible. Thank you!

## Makefile

A simple **Makefile** is provided. This file can be used to compile LaTe in the Linux system in which this repository was cloned, with `gcc`. Any improvement to this file is highly welcome.

Additional targets are also defined; in particular:
- `compilePCdebug`, to compile for the current platform, with `gcc` and the flag `-g` to generate debug informations, to be used with `gdb`.
- `compileAPU`, as we also used **LaTe** to perform wireless latency measurements on [PC Engines APU1D embedded boards](https://pcengines.ch/apu1d.htm), running [OpenWrt](https://github.com/francescoraves483/OpenWrt-V2X), we defined an additional target to cross-compile LaTe for the boards. This command should work when targeting any **x86_64** embedded board running **OpenWrt**, after the toolchain has been properly set up (tested with OpenWrt 18.06.1). If you want to cross-compile LaTe for other Linux-based platforms, you will need to change the value of **CC_EMBEDDED** inside the Makefile with the compiler you need to use.
- `compileAPUdebug`, as before, but with the `-g` flag to generate debug informations for `gdb`.

**LaTe** has been tested on Linux kernel versions 4.14.63 and 4.15.0 and it is currently using the [**Rawsock library, version 0.2.1**](https://github.com/francescoraves483/Rawsock_lib).

## How to compile

Clone the master branch of the repository:
```
git clone https://github.com/francescoraves483/LaMP_LaTe.git
cd LaMP_LaTe
```
Compile:
```
make
```

The executable is called `LaTe`.

\* In the current version, only **LaMP** over **IPv4** and **UDP** is supported, but we plan to implement other protocols in the future.

# LaMP specifications

The full LaMP specifications can be found here: [LaMP specifications, revision 1.0](./LaMP/LaMP_specifications_rev1.0.pdf).

# LaTe user guide (v0.1.1-beta+)

## Modes

LaTe supports different modes, based on the **client-server** paradigm, which is the one supported by LaMP.
The first command line argument should be a letter corresponding to the desired mode:

- **-c \<destination address\>**: client mode
- **-s**: server mode
- **-l**: client mode, binding to a loopback interface
- **-m**: server mode, binding to a loopback interface
  
Alternatively, it is possible to specify:
- **-h**: to print the long version of the help, directly within LaTe, including the available interfaces and their internal indeces, to be passed to LaTe using _-I_.
- **-v**: to print additional version information

*-v* or *-h* should be used alone, without any additional parameter.

When *-c* is selected, the destination address must be specified, depending on the protocol. **In UDP mode, this address corresponds to the destination IPv4 address.**

When a client (either *-c* or *-l*) is selected, just after *-c/-l*, a mode has to be specified:
- **-B** for the standard ping-like bidirectional mode, working like ICMP Echo Request/Reply, but in a client-server fashion (read also the [LaMP specifications](./LaMP/LaMP_specifications_rev1.0.pdf)).
- **-U** for a highly experimental unidirectional mode, in which the client sends a LaMP packet to the server, and the latter tries to compute the latency, based on its own timestamp and on the one embedded by the client inside the packet, without the need of generating a reply. It works only if there is an external way of keeping the devices’ clocks perfectly synchronized, with under-ms precision. As this is not an easy task, this mode shall remain experimental and it should never be preferred over the ping-like one.

## Protocols

A protocol should be then specified. As of now, LaTe supports latency measurements only with UDP, over IPv4, but other protocols will be implemented in the future.

Protocols are specified through specific arguments. Supported protocols are:
- **-u**: UDP (LaMP over UDP)

## Client and server specific options

Then, client and sever specific options should be specified, according to the following table:

### Mandatory client options

| Argument | Value | Description | Default value |
| -------- | ----- | ----------- | ------------- |
-M | Destination MAC address | Specifies the destination MAC address. Mandatory only if socket is RAW (*-r* is selected) and protocol is UDP | - |


### Optional client options

| Argument | Value | Description | Default value |
| -------- | ----- | ----------- | ------------- |
| -n | Total number of packets to be sent, integer, *> 0* | Specifies how many packets to send. | 600 |
| -t | Time interval in ms, integer, *> 0* | Specifies the periodicity, in milliseconds, to send at. | 100 |
| -f | Filename, without extension | Print the report to a CSV file other than printing it on the screen. The default behaviour will append to an existing file; if the file does not exist, it is created. | - |
| -o (valid only with *-f*) | - | Instead of appending to an existing file, overwrite it. | - |
| -p | Payload size in B, *>= 0* | specify a LaMP payload length, in bytes, to be included inside the packet. | 0 |
| -r | - | Use raw sockets, if supported for the current protocol. When '-r' is set, the program tries to insert the LaMP timestamp in the last possible instant before sending. _sudo_ (or proper permissions) is required in this case. | (non raw) |
| -A | Access category, string: **BK** or **BE** or **VI** or **VO** | Forces a certain EDCA MAC access category to be used. This option requires a pathed kernel to work (a warning will be printed about this), but it has been successfully used to perform latency measurements over 802.11p, using a patched version of OpenWrt, i.e. using both OpenC2X-Embedded (https://github.com/florianklingler/OpenC2X-embedded) and OpenWrt-V2X (https://github.com/francescoraves483/OpenWrt-V2X). | (not set, i.e. AC_BE) |
| -L | Latency type, char: **u** or **r** | Select latency type: user-to-user or KRT (Kernel Receive Timestamp). Additional types will probably be added in future versions of the program.| u |
| -I | Interface index (returned by the _-h_ option) | Internal index of the interface to be used for the latency measurement session. | 0 |
| -e | - | Bind to a non-wireless interface instead of looking for wireless interfaces. | (wireless) |
| -p | Port, integer, *> 0* and *< 65535* | Specifies the port to be used. Can be specified only if protocol is UDP. | 46000 |


### Mandatory server options (none)

| Argument | Value | Description | Default value |
| -------- | ----- | ----------- | ------------- |
| - | - | - | - |


### Optional server options

| Argument | Value | Description | Default value |
| -------- | ----- | ----------- | ------------- |
| -t | Timeout in ms, integer, *> 0* | Specifies the timeout after which the connection should bec onsidered lost (minimum value: 1000 ms, otherwise 1000 ms will be automatically set). | 4000 |
| -r | - | Use raw sockets, if supported for the current protocol. When '-r' is set, the program tries to insert the LaMP timestamp in the last possible instant before sending. _sudo_ (or proper permissions) is required in this case. | (non raw) |
| -A | Access category, string: **BK** or **BE** or **VI** or **VO** | Forces a certain EDCA MAC access category to be used. This option requires a pathed kernel to work (a warning will be printed about this), but it has been successfully used to perform latency measurements over 802.11p, using a patched version of OpenWrt, i.e. using both OpenC2X-Embedded (https://github.com/florianklingler/OpenC2X-embedded) and OpenWrt-V2X (https://github.com/francescoraves483/OpenWrt-V2X). | (not set, i.e. AC_BE) |
| -L | Latency type, char: **u** or **r** | Select latency type: user-to-user or KRT (Kernel Receive Timestamp). Please note that the server supports this parameter only when in unidirectional mode. If a bidirectional INIT packet is received, the mode is completely ignored. Additional types will probably be added in future versions of the program.| u |
| -d | - | Set the server in *continuous daemon mode*: as a session is terminated, the server will be restarted and will be able to accept new packets from other clients, in a new session | (off) |
| -I | Interface index (returned by the _-h_ option) | Internal index of the interface to be used for the latency measurement session. | 0 |
| -e | - | Bind to a non-wireless interface instead of looking for wireless interfaces. | (wireless) |
| -p | Port, integer, *> 0* and *< 65535* | Specifies the port to be used. Can be specified only if protocol is UDP. | 46000 |

## Example of usage

These examples of usage are also displayed when calling the program with the *-h* option.

### Non-RAW sockets and UDP
*Client (port 7000, ping-like, 100 packets, one packet every 100 ms, LaMP payload size: 700 B):*

`LaTe -c 192.168.1.180 -p 7000 -B -u -t 100 -n 100 -P 700`

*Server (port 7000, timeout: 5000 ms):*

`LaTe -s -p 7000 -t 5000 -u`

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



## Important notes

Few important notes about **LaTe** are listed below:

- In order to use raw sockets, root privileges are needed.
- Time intervals less or equal to 0 ms are not supported and will generate an error (i.e., after *-t* a number *>= 1* should be specified), as they would make no sense in this context.
- When in UDP mode, LaMP payloads are supported up to 1448 B, in order not to exceed the Ethernet MTU, which would case fragmentation in non-raw mode and a transmission error in raw mode.
- When raw sockets are used (i.e. *-r* is specified), a destination MAC address should be specified with *-M*.
- Specifying a port is required when working with LaMP over UDP (i.e. when using the *-u* option). If no port is specified, the program will use `46000` as default value.
- The server will adapt its mode (ping-like or unidirectional) depending on the packets it receives from the client. Therefore, specifying *-B* or *-U* for a server will have no effect.
- When in non-raw mode, the destination MAC address is not required. If specified with *-M*, however, it won’t generate an error, but it will be simply ignored by LaTe.

## Latency types

Two latency types are supported as of now. They are all computed thanks to seconds and microseconds timestamps.

**User-to-user (‘u’)**: in which the sender timestamp (i.e. the client timestamp) is placed in the packet just before passing it to the send system call (for instance, just before passing the LaMP packet to a send call over a *SOCK_DRAM* socket when in UDP mode, or before passing the whole raw packet, containing IPv4, UDP and LaMP, to a send call related to an *ETH_P_ALL* raw socket); the receiver timestamp is instead obtained from the real-time clock as soon as the packet has been received and it has been parsed, checking if it is of interest (i.e. if it is really LaMP, if it is has the correct ID and if it is actually the expected one).

**KRT - Kernel Receive Timestamp (‘r’)**: this mode is the one used by iputils ping (if this is wrong, please correct us) to compute the latency with ICMP Echo packets, at least in a standard case. The sender timestamp is obtained like in the User-to-user mode, but the receiver one is obtained directly from the Linux kernel, as every packet is received. These timestamps are still software timestamps, and, according to the kernel documentation, they should be “generated just after a device driver hands a packet to the kernel receive stack”. This allows to reduce the application latency contribution from the receiver side.

## How does the client/server work?

When the client has been launched, it will try to transmit initialization packets to the specified server. 

If no server is found, the client will stop sending these packets after a certain number of attempts and will terminate its execution.

If a server has been found, the client will start sending LaMP packets to the server and the measurement session will start.

In **ping-like** mode, the server will try to reply at each client request, and the client will look at the replies to compute the statistics returned by the program.

In **unidirectional (experimental) mode**, the server will try to guess the latency from the timestamp embedded by the client inside each LaMP packet, sending back to the client the report data as the session finishes, for visualization.

At the end of the session, the average, minimum and maximum latency values will be reported, together with the number of lost packets and an indicative measure of the out-of-order count, as the number of times a decreasing sequence number is detected in the received packets (i.e. in the received replies). If *-f* is specified, this data will also be written to a *.csv* file.

Both client and server have a timeout set on the sockets they use to receive and send data. After few seconds of inactivity, they will declare the connection as terminated and terminate their execution (or, in case a daemon mode server is launched, a new session will start).

Both the client and the server follow the [LaMP specifications](./LaMP/LaMP_specifications_rev1.0.pdf)


## How does the server daemon mode work?

When *-d* is specified on a server, a special daemon mode will be activated.

When using this mode, the server will not terminate its execution after a session is correctly (or badly, due to an error) terminated, but it will, instead, start again listening for a client willing to connect. This allows to launch a single server for multiple successive client connections and measurement sessions.

In order to correctly terminate the server, the user can send the SIGUSR1 signal to the LaTe process on which the server is running, using `kill -s USR1 <pid>`.

The `<pid>` is an integer number univocally identifying each process (i.e. each “running program”)  running on the system.

The `<pid>` of the LaTe server program can be found by means of the `ps` utility (if it is not listed, use `ps -A`).

After giving the termination command, the current session will run until it will finish, then the program will be terminated as a normal, non-daemon, server.

# Version information

The current version is **0.1.1-beta**, using **LaMP rev 1.0** and **Rawsock library v0.2.1**.

Developed in **Politecnico di Torino**, licensed under **GPLv2**.

This is **open source**: you should never be charged for using or downloading LaTe and the source code should always be made available to you.