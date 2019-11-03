#!/bin/bash
git submodule update --init --recursive
sudo apt-get install gcc git wget make libncurses-dev flex bison gperf python python-pip python-setuptools python-serial python-cryptography python-future
curl -L https://dl.espressif.com/dl/xtensa-esp32-elf-linux64-1.22.0-80-g6c4433a-5.2.0.tar.gz | tar xvz
sudo usermod -a -G dialout $USER
sudo pip install -r esp-idf/requirements.txt
