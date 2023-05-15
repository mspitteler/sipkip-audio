#!/bin/bash

old_IFS="$IFS"
IFS=$'\n'

global_header_file="../main/audio.h"
global_cmake_file="../main/audio_identifiers.cmake"
opus_files=`find . -name \*.opus`

echo "/**
 * This file contains all the includes for the opus audio fragments, which are in
 * files containing the concatinated opus packets, and files containing the sizes of these opus packets.
 */" > "$global_header_file"
 
echo "target_link_libraries(\${COMPONENT_LIB} INTERFACE" > "$global_cmake_file"

for opus_file in $opus_files; do
    header_file="${opus_file%.opus}.h"
    echo -n "const " > "$header_file"
    xxd --include "$opus_file" >> "$header_file"

    echo -n "                      \"-u " >> "$global_cmake_file"
    head -n 1 "$header_file" | awk -F "[^_a-zA-Z0-9]" "{printf \$4}" >> "$global_cmake_file"
    echo '"' >> "$global_cmake_file"

    echo -n "const " > "$header_file.tmp"
    xxd --include "${opus_file}_packets" >> "$header_file.tmp"
    cat "$header_file.tmp" >> "$header_file"

    echo -n "                      \"-u " >> "$global_cmake_file"
    head -n 1 "$header_file.tmp" | awk -F "[^_a-zA-Z0-9]" "{printf \$4}" >> "$global_cmake_file"
    echo '"' >> "$global_cmake_file"


    rm "$header_file.tmp"
    echo "#include \"audio/${header_file#./}\"" >> "$global_header_file"
done

echo ')' >> "$global_cmake_file"

IFS="$old_IFS"
