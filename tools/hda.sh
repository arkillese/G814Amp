# Source from the G814Amp folder.  v <cad> <nid> <verb12> <payload8> sends a 12-bit verb;
# v4 <cad> <nid> <verb4> <payload16> sends a 4-bit verb (amp gain/mute, format).
T=./build/probe-tool
_send(){ $T verb $1; ioreg -l -w0 -r -c G814Amp -d1 | grep -E '"codec-(resp|result)"' | sed -E 's/.*"codec-resp" = ([0-9]+)/\1/; s/.*"codec-result" = //' | tr '\n' ' ' | awk '{ if ($1 ~ /^[0-9]+$/) printf "resp=0x%08x %s\n", $1, $2; else printf "resp=0x%08x %s\n", $2, $1 }'; }
v(){ _send $(( ($1<<28) | ($2<<20) | ($3<<8) | $4 )); }
v4(){ _send $(( ($1<<28) | ($2<<20) | ($3<<16) | $4 )); }
