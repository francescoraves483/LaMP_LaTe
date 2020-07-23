**Graphite sample configuration - to receive data from LaTe with the -g option**

This directory contains a sample configuration file for the Graphite component responsible for receiving the metrics from external applications. This component is called **Carbon** and it is made up of one or more daemons for metrics processing.

More details can be found [here](https://github.com/graphite-project/carbon) or in the [official Graphite documentation](https://graphite.readthedocs.io/en/latest/config-carbon.html).

This directory contains a sample **storage-aggregation.conf** file, which is used by Graphite to properly aggregate data points when moving to a lower precision or to a larger time scale.
This file already sets up the right way to aggregate the data coming from LaTe, and it can be used as it is, by copying it to the Carbon configuration files directory (by default in: `/opt/graphite/conf/`), or as an example to properly build your own Carbon aggregation configuration.

Before starting a test with LaTe and Graphite (and possibly also Grafana to visualize the data), remember to set the desired retention rate in **storage-schemas.conf** (more details are available in the [official Graphite documentation](https://graphite.readthedocs.io/en/latest/config-carbon.html)).