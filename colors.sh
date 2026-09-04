#!/bin/sh
# Show every colour and text attribute the terminal can draw.
#
# Uses only ASCII characters: the bundled font covers 32-126, so block-drawing
# characters would come out as '?'. Colour swatches are spaces with a
# background colour instead.

esc()   { printf '\033[%sm' "$1"; }
reset() { printf '\033[0m'; }

names='black red green yellow blue magenta cyan white'

# Four names per row, printed in the colour they name.
swatch_names() {
    prefix=$1      # 3 for normal foreground, 9 for bright
    offset=$2      # 0 or 8, for the number shown
    i=0
    for name in $names; do
        esc "$prefix$i"
        printf '  %2d %-8s' "$((i + offset))" "$name"
        reset
        i=$((i + 1))
        [ "$i" -eq 4 ] && echo
    done
    echo
}

# Eight numbered blocks, coloured by background.
swatch_blocks() {
    prefix=$1      # 4 for normal background, 10 for bright
    offset=$2
    i=0
    while [ "$i" -lt 8 ]; do
        esc "$prefix$i"
        printf ' %2d ' "$((i + offset))"
        reset
        i=$((i + 1))
    done
    echo
}

echo
echo "Foreground"
echo
echo "  normal"
swatch_names 3 0
echo "  bright"
swatch_names 9 8

echo
echo "Background"
echo
printf '  normal  '; swatch_blocks 4 0
printf '  bright  '; swatch_blocks 10 8

echo
echo "Attributes"
echo
printf '  normal  '
esc 1;     printf 'bold';           reset; printf '  '
esc 4;     printf 'underline';      reset; printf '  '
esc 7;     printf 'reverse';        reset; printf '  '
esc '1;4'; printf 'bold underline'; reset
echo

echo
echo "Text on a coloured background"
echo
printf '  '
esc '33;44';   printf ' yellow on blue ';    reset; printf '  '
esc '30;42';   printf ' black on green ';    reset; printf '  '
esc '1;37;41'; printf ' bold white on red '; reset
echo
echo
