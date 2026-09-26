#!/bin/bash

# Variable declarations
VAR="world"
ANOTHER_VAR="codelenses"

# Single-quoted string literal (no expansion)
single_str='literal $VAR without expansion'

# Double-quoted string with variable expansions
double_str="hello $VAR from ${ANOTHER_VAR}"

# ANSI-C quoting
ansi_str=$'line 1\nline 2\ttab'

# Heredoc with variable expansions
cat <<EOF
Greeting: $VAR
Project: ${ANOTHER_VAR}
EOF

# Quoted heredoc without expansion
cat <<'EOF'
Literal $VAR without expansion
EOF

# Commands using quoted arguments
echo "Done: $VAR"
printf '%s\n' 'single quoted message'
