#!/bin/sh
set -eu

controller_test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
controller_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/armsx-controller-test.XXXXXX")
trap 'rm -f "$controller_test_dir/controller-motion.jar" "$controller_test_dir/android-api.jar"; rmdir "$controller_test_dir"' EXIT HUP INT TERM
cd "$controller_test_root"

kotlinc android/app/src/main/java/com/armsx2/input/ControllerAxisPolicy.kt \
    android/app/src/main/java/com/armsx2/input/ControllerMotion.kt \
    tests/controller_motion/AndroidInput.kt tests/controller_motion/ControllerMotionTest.kt \
    -include-runtime -d "$controller_test_dir/controller-motion.jar"
java -jar "$controller_test_dir/controller-motion.jar"

if [ "$#" -gt 0 ]; then
    kotlinc android/app/src/main/java/com/armsx2/input/ControllerAxisPolicy.kt \
        android/app/src/main/java/com/armsx2/input/ControllerMotion.kt \
        -classpath "$1" -d "$controller_test_dir/android-api.jar"
fi
