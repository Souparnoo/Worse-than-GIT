#!/bin/bash

function wtgit(){
DIR=$PWD
export dir=$DIR
cd ~/wtgit/bin || echo "Error"
./main "$@"
cd "$DIR" || echo "Error"
}   