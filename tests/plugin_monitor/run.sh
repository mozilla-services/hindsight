#!/usr/bin/env bash
rm -rf output
hindsight_cli hindsight.cfg 7
rc=$?; if [[ $rc != 6 ]]; then exit $rc; fi
