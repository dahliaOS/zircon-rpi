#!/bin/bash

# Script for logging bluetooth information in formal testing sessions.

# Usage: scripts/bt-log.sh test-name-1

# Starts up a bt-snoop wireshark session showing current connected device bt snoop.
# Opens a tmux session that is split between a log file (test-notes.log) and the
# output of `fx syslog`
#
# Will create outputs in $FUCHSIA_DIR/local/test-name-1:
#   - system.log (output from syslog)
#   - bt-snoop.pcap (same as what is in wireshark)
#   - test-notes.log

if [ $# -lt 1 ]; then
  echo "Usage: bt-log.sh <tag>"
  exit 1
fi

fx wait

BASE_PATH=${FUCHSIA_DIR}/local/$1

mkdir -p $BASE_PATH

fx shell bt-snoop-cli | tee ${BASE_PATH}/bt-snoop.pcap | nohup wireshark-gtk -k -i - > ${BASE_PATH}/wireshark.out 2>&1 &

tmux new-session -d -s $1
tmux rename-window "$1"
tmux send-keys "fx syslog | tee ${BASE_PATH}/system.log" 'C-m'
tmux split-window -h
tmux send-keys "touch ${BASE_PATH}/test-notes.log; vim ${BASE_PATH}/test-notes.log" 'C-m'
tmux split-window -h
tmux attach-session -t $1
