**Graphite and Grafana quick start scripts**

This folder contains two bash scripts which can be used to more easily start and stop Grafana and Graphite under Linux, in case you want to try out Graphite by means of its [containerized version](https://graphite.readthedocs.io/en/latest/install.html#docker).

Using this containerized version can be an effective and quick way of testing and working with Graphite, including use cases in which the metrics come from LaTe, via the `-g` option.

Moreover, [Grafana](https://grafana.com/) can provide advanced tools to analyze and visualize any metric stored on Graphite (and on a number of other databases).

Before launching any of these scripts, you need to install:
- Docker (to install it on Ubuntu see: https://docs.docker.com/engine/install/ubuntu/ - to install it on other platforms, you can refer to: https://docs.docker.com/engine/install/ )
- Grafana (to install it on Ubuntu/Debian see: https://grafana.com/docs/grafana/latest/installation/debian/ - to install it on other platforms, instead, you can have a look at: https://grafana.com/docs/grafana/latest/installation/ )

After installing these prerequisites, you can use the two scripts:
- `graphite_launcher.sh`, which will look for an already available Graphite Docker container. If it is available, it will simply start it, making the Graphite web UI (ngnix) accessible on port **48002** and keeping all the other ports as documented [here](https://github.com/graphite-project/docker-graphite-statsd). If it is not available, it will try to automatically create it, and then start it. In order to make it easier to modify the configuration files and access the most relevant Graphite files (such as the ones containing the Whisper timeseries data), the most important directories are made accessible in **`/usr/graphite`**. For instance, the configuration files should be accessible in **`/usr/graphite/graphite-conf`**.
This script will also start the Grafana service (`grafana-server`).
After launching this script, if the procedure was completed without errors, both Graphite and Grafana (which should be accessible on port **3000**, by default) should be up and running.
- `graphite_killer.sh`, which will stop a running Graphite container, launched through the previous script, and the Grafana service (`grafana-server`).

In order to "connect" Grafana with Graphite, you can refer to the documentation available [here](https://grafana.com/docs/grafana/latest/features/datasources/graphite/).

**Note:** these scripts are quite simple, and they can be used as a base to build more complex scripts to automate the procedure of starting and stopping Grafana and Graphite. It is also planned to enhance them in the future, by adding, for instance, more options and some more error handling in case something goes wrong.
As always, we highly welcome any contribution, improvement, bug report or suggestion to improve both LaMP/LaTe and these scripts/examples. Thank you!