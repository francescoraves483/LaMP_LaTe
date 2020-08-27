#!/bin/bash

read -p "Press ENTER to start the graphite docker container, together with Grafana..."

echo "Starting Graphite..."

# Create the container if it does not exist yet, otherwise, just start it
docker ps -a | grep graphite >/dev/null 2>&1
if [ $? -eq 1 ]; then
	sudo mkdir -p /usr/graphite
	sudo mkdir -p /usr/graphite/graphite-conf
	sudo mkdir -p /usr/graphite/graphite-storage
	sudo mkdir -p /usr/graphite/webapp/graphite/functions/custom
	sudo mkdir -p /usr/graphite/nginx
	sudo mkdir -p /usr/graphite/statsd-conf
	sudo mkdir -p /usr/graphite/logrotate.d
	sudo mkdir -p /usr/graphite/var/log
	sudo mkdir -p /usr/graphite/var/lib/redis

	docker volume create --driver local -o o=bind -o type=none -o device=/usr/graphite/graphite-conf graphite-conf
	docker volume create --driver local -o o=bind -o type=none -o device=/usr/graphite/graphite-storage graphite-storage
	docker volume create --driver local -o o=bind -o type=none -o device=/usr/graphite/webapp/graphite/functions/custom graphite-webapp
	docker volume create --driver local -o o=bind -o type=none -o device=/usr/graphite/nginx graphite-nginx
	docker volume create --driver local -o o=bind -o type=none -o device=/usr/graphite/statsd-conf graphite-statsd-conf
	docker volume create --driver local -o o=bind -o type=none -o device=/usr/graphite/logrotate.d graphite-logrotate
	docker volume create --driver local -o o=bind -o type=none -o device=/usr/graphite/var/log graphite-varlog
	docker volume create --driver local -o o=bind -o type=none -o device=/usr/graphite/var/lib/redis graphite-redis

	docker run -d \
		--name graphite \
		--restart=always \
		-p 48002:80 \
		-p 2003-2004:2003-2004 \
		-p 2023-2024:2023-2024 \
		-p 8125:8125/udp \
		-p 8126:8126 \
		--mount 'type=volume,source=graphite-conf,target=/opt/graphite/conf' \
		--mount 'type=volume,source=graphite-storage,target=/opt/graphite/storage' \
		--mount 'type=volume,source=graphite-webapp,target=/opt/graphite/webapp/graphite/functions/custom' \
		--mount 'type=volume,source=graphite-nginx,target=/etc/nginx' \
		--mount 'type=volume,source=graphite-statsd-conf,target=/opt/statsd/config' \
		--mount 'type=volume,source=graphite-logrotate,target=/etc/logrotate.d' \
		--mount 'type=volume,source=graphite-varlog,target=/var/log' \
		--mount 'type=volume,source=graphite-redis,target=/var/lib/redis' \
		graphiteapp/graphite-statsd
else
	docker start graphite
fi

echo "Starting Grafana..."
sudo /bin/systemctl start grafana-server

echo "Done. The Carbon receiver - plaintext is listening on port 2003."