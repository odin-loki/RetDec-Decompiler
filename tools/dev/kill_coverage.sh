#!/usr/bin/env bash
pkill -f "ninja.*core-coverage" 2>/dev/null
pkill -f "core-coverage" 2>/dev/null
echo "killed coverage build processes"
