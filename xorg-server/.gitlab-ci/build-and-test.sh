#!/bin/bash

set -e
set -o xtrace

check_piglit_results ()
{
    local EXPECTED_RESULTS=build/test/piglit-results/$1
    local DEPENDENCY=build/$2

    if ! test -e $DEPENDENCY; then
	return
    fi

    if test -e $EXPECTED_RESULTS; then
	return
    fi

    echo Expected $EXPECTED_RESULTS does not exist
    exit 1
}

meson -Dc_args="-fno-common" -Dprefix=/usr -Dxephyr=true -Dwerror=true $MESON_EXTRA_OPTIONS build/

ninja -j${FDO_CI_CONCURRENT:-4} -C build/ dist

export PIGLIT_DIR=/root/piglit XTEST_DIR=/root/xts
ninja -j${FDO_CI_CONCURRENT:-4} -C build/ test

check_piglit_results xephyr-glamor hw/kdrive/ephyr/Xephyr.p/ephyr_glamor.c.o
check_piglit_results xvfb hw/vfb/Xvfb
check_piglit_results xwayland hw/xwayland/Xwayland
