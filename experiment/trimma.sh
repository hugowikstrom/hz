#!/usr/bin/env bash
# Bygger nvjpg med givna modellval och mäter på samma urval.
set -u
for val in "$@"; do
    gcc -O2 -w $val -o /tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/nvjpg-t src/nvjpg.c || { echo "$val: BYGGFEL"; continue; }
    cp /tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/nvjpg-t src/nvjpg.trim
    printf '%-34s ' "${val:-grund}"
    sed 's|./src/nvjpg|./src/nvjpg.trim|' experiment/jpg-storlek.sh > /tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/st.sh
    bash /tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/st.sh < experiment/urval-jpg.txt
done
rm -f src/nvjpg.trim
