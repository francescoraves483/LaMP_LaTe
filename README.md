![](./docs/pics/LaTe_logo_beta_small.png)

**powered by...**

![](./docs/pics/LaMP_logo.png)

**LaTe** - Flexible, client-server, multi-protocol* **La**tency **Te**ster, based on the custom **LaMP** protocol (**La**tency **M**easurement **P**rotocol) and running on **Linux** - _version 0.1.6-beta-development_

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

The additional **Makefile_termux** makefile can be used when building the LaTe version supporting AMQP on a non-rooted **Android** device, using Termux.
When installing Qpid Proton, you can choose to install it inside `/data/data/com.termux/files/home/libs`, which will make root privileges unnecessary when running `make install`.
This makefile, when running `make -f Makefile_termux compilePCfull` will look for Qpid Proton inside `/data/data/com.termux/files/home/libs`, allowing you to compile also the full version of LaTe.
In this particular case, before launching LaTe, after compiling it, remember to add the new library path to `LD_LIBRARY_PATH`:
```
export LD_LIBRARY_PATH=/data/data/com.termux/files/home/libs
```

Please note that in order to compile Qpid Proton on Android, you may need to manually patch the library. For your conveniency, an already patched QPid Proton C library (version 0.30.0) is available [here](https://github.com/francescoraves483/qpid-proton).

**LaTe** has been extensively tested on Linux kernel versions 4.14, 4.15, 5.0 and 5.4 and it is currently using the [**Rawsock library, version 0.3.3**](https://github.com/francescoraves483/Rawsock_lib).

**How to compile**

Clone this repository (`--recursive` is needed in order to clone the Rawsock library submodule too):
```
git clone --recursive https://github.com/francescoraves483/LaMP_LaTe.git
git checkout development --recurse-submodules
cd LaMP_LaTe
```
Compile:
```
make
```

The executable is called `LaTe`.

\* In the current version, only **LaMP** over **IPv4** and **UDP** is supported, but we plan to implement other protocols in the future.

**Docker images**

Docker images are also available, launching a LaTe server in daemon mode (see the documentation for more information). You can download them from [DockerHub](https://hub.docker.com/u/francescoraves483).

At the moment two images are available: one for testing over wired interfaces and one for testing over wireless interfaces.

**Warning:** Docker images may not include the latest version of LaTe.


![](./docs/pics/EU_flag.jpg)

*Please have a look also at disclaimer.txt, as this work is also included in the European Union Horizon 2020 project 5G-CARMEN co-funded by the EU*