#!/bin/bash
if [ -z "$VITASDK" ]; then
    echo "VITASDK environment variable is not set. Please set it before running this script."
    exit 1
fi

LIB_DIR="$VITASDK/arm-vita-eabi/lib"

if [ ! -d "$LIB_DIR" ]; then
    echo "Directory $LIB_DIR does not exist."
    exit 1
fi

ls -l "$LIB_DIR" > available_libs.txt
echo "Library list saved to available_libs.txt"