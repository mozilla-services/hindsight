#!/usr/bin/env bash
rm -rf output
hindsight hindsight.cfg 7 2>&1 | grep -q success
