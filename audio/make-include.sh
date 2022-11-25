#!/bin/bash

old_IFS="$IFS"
IFS=$'\n'

opus_files=`find . -name \*.opus`

for opus_file in $opus_files; do
    header_file="${opus_file%.opus}.h"
    echo -n "const " > "$header_file"
    xxd --include "$opus_file" >> "$header_file"
    echo -n "const " >> "$header_file"
    xxd --include "${opus_file}_packets" >> "$header_file"
done

IFS="$old_IFS"
