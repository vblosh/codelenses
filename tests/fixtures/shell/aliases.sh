#!/bin/bash

# Alias declarations
alias ll='ls -la'
alias gst="git status"
alias grep='grep --color=auto'

list_files() {
    ll /tmp
    gst
}

alias cls=clear

main() {
    list_files
    cls
}
