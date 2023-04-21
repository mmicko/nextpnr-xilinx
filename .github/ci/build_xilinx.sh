#!/bin/bash

function get_dependencies {
    :
}

function build_nextpnr {
    mkdir build
    pushd build
    cmake .. -DARCH=xilinx -DBUILD_GUI=on -DUSE_IPO=off
    make nextpnr-xilinx -j`nproc`
    popd
}

function run_tests {
}

function run_archcheck {
}
