![](./docs/pics/LaTe_logo_beta_small.png)

**powered by...**

![](./docs/pics/LaMP_logo.png)

**LaTe** - Flexible, client-server, multi-protocol* **La**tency **Te**ster, based on the custom **LaMP** protocol (**La**tency **M**easurement **P**rotocol) and running on **Linux** - _version 0.1.0-beta_

This repository is the main one for what concerns both **LaTe** and the **LaMP** custom protocol, including its specifications.

As we are still in a beta stage, we highly welcome any contribution, improvement, bug report or suggestion, both to **LaTe** and to the **LaMP** specifications.
Other than using GitHub, you can freely write to <francescorav.es483@gmail.com>: we will try to reply you as soon as possible. Thank you!

You can find the **LaMP** specifications and the full **LaTe** documentation here: 
https://francescoraves483.github.io/LaMP_LaTe/

A simple **Makefile** is provided. This file can be used to compile LaTe in the Linux system in which this repository was cloned, with `gcc`. Any improvement to this file is highly welcome.

Additional targets are also defined; in particular:
- `compilePCdebug`, to compile for the current platform, with `gcc` and the flag `-g` to generate debug informations, to be used with `gdb`.
- `compileAPU`, as we also used **LaTe** to perform wireless latency measurements on [PC Engines APU1D embedded boards](https://pcengines.ch/apu1d.htm), running [OpenWrt](https://github.com/francescoraves483/OpenWrt-V2X), we defined an additional target to cross-compile LaTe for the boards. This command should work when targeting any **x86_64** embedded board running **OpenWrt**, after the toolchain has been properly set up (tested with OpenWrt 18.06.1). If you want to cross-compile LaTe for other Linux-based platforms, you will need to change the value of **CC_EMBEDDED** inside the Makefile with the compiler you need to use.
- `compileAPUdebug`, as before, but with the `-g` flag to generate debug informations for `gdb`.

**LaTe** has been tested on Linux kernel versions 4.14.63 and 4.15.0 and it is currently using the [**Rawsock library, version 0.2.0**](https://github.com/francescoraves483/Rawsock_lib).

**How to compile**

Clone this repository:
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