# Append to ~/.bashrc after its interactive-shell check.
# Do not replace the rest of ~/.bashrc.
if [ -t 0 ]; then
    stty -ixon
fi
