#!/bin/bash

read -p "Press ENTER to stop the graphite docker container, together with Grafana..."

echo "Stopping Graphite..."
sudo docker stop graphite

echo "Stopping Grafana..."
sudo /bin/systemctl stop grafana-server

echo "Done."