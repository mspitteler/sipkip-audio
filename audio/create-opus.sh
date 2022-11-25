#!/bin/bash

old_IFS="$IFS"
IFS=$'\n'

gcc opusenc.c -o opusenc -lopus

wav_files=`find '../gesplitste geluiden' -name \*.wav`
first=true
for i in $wav_files; do
    opus_file="${i%.wav}.opus"
    opus_file="./${opus_file#"../gesplitste geluiden/"}"
    opus_packets_file="${opus_file}_packets"
    pcm_file="${opus_file%.opus}.pcm"

    mkdir -p "`dirname "$pcm_file"`"
    sox "$i" -t raw -r 48000 -b 16 -c 1 -L -e signed-integer "$pcm_file"
    ./opusenc "$pcm_file" "$opus_file" "$opus_packets_file"
    rm "$pcm_file"
done

rm opusenc

IFS="$old_IFS"
