#!/bin/bash

# Parenthesized subshell
(
    sub_var="in_subshell"
    echo "$sub_var"
    cd /tmp && pwd
)

# Command substitution
current_dir=$(pwd)
old_style=`whoami`

# Function with subshell body
run_isolated() (
    isolated_var=42
    echo "$isolated_var in isolated function"
)

# Process substitution
diff <(cat file1.txt) <(cat file2.txt)
