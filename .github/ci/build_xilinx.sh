#!/bin/bash

function get_dependencies {
    :
}

function build_nextpnr {
    cmake . -DARCH=xilinx -DBUILD_GUI=on -DUSE_IPO=off
    make bbasm nextpnr-xilinx -j`nproc`
}

function run_tests {
    export XRAY_DIR=${GITHUB_WORKSPACE}/.prjxray
    export PATH=${GITHUB_WORKSPACE}/.yosys/bin:$PATH
    python3 xilinx/python/bbaexport.py --device xc7a35tcsg324-1 --bba xilinx/xc7a35t.bba
    ./bba/bbasm --l xilinx/xc7a35t.bba xilinx/xc7a35t.bin 
    pushd xilinx/examples/arty-a35
    export XRAY_UTILS_DIR=${XRAY_DIR}/utils
    export XRAY_TOOLS_DIR=${XRAY_DIR}/tools
    sh blinky.sh
    sh attosoc.sh
    popd
}

function run_archcheck {
    :
}
