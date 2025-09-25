#!/usr/bin/env bash

# Copyright (C) 2024 Hong Kong University of Science and Technology
# 
# This repository is used for the Computer Networks (ELEC 3120) course taught
# at Hong Kong University of Science and Technology.
# 
# No part of the project may be copied and/or distributed without the express
# permission of the course staff. Everyone is prohibited from releasing their
# forks in any public places.


SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

sudo bash -c 'echo "nameserver 8.8.8.8" > /etc/resolv.conf'

sudo apt-get update

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential vim \
    emacs tree tmux git gdb valgrind python3-dev libffi-dev libssl-dev \
    clang-format iperf3 tshark iproute2 iputils-ping net-tools tcpdump cppcheck

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y python3 python3-pip \
    python-tk libpython3.10-dev libcairo2 libcairo2-dev pre-commit zip

pip3 install --upgrade pip
pip3 install -r /vagrant/foggytcp/setup/requirements.txt

echo "cd /vagrant/foggytcp/" >> /home/vagrant/.profile
