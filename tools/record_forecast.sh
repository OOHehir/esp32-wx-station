#!/usr/bin/env bash
# Re-record the Open-Meteo fixture the parser tests run against.
#
#     ./tools/record_forecast.sh
#
# The tests must not reach the network -- a test that fails when the weather changes is
# not testing the parser (PLAN 5b). So the response is recorded here, by hand, when the
# query in PLAN 4a changes, and committed as a C string literal.
#
# Re-recording invalidates the values asserted in test/host/test_wx_parse.c: the
# timestamps, temperatures and codes there were read off the old response. Expect to
# update them in the same commit, and read them off the new JSON rather than trusting
# that they still hold.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out_json="$repo_root/test/host/fixtures/forecast_dublin.json"
out_h="$repo_root/test/host/fixtures/forecast_dublin.h"

# The window, pinned rather than relative.
#
# PLAN 4a's query asks for `forecast_hours=13&forecast_days=2`, which starts at the hour
# in progress -- so what the fixture covers depends on the time of day it was recorded at.
# Recorded one morning it holds thirteen daylit hours in a single date, and the midnight
# rollover and the night hours the parser tests lean on quietly disappear. That is a
# fixture getting weaker without anybody noticing, which is the whole failure mode a
# committed fixture exists to prevent.
#
# So the window is asked for explicitly: 20:00 local today to 08:00 tomorrow, thirteen
# hours inclusive, crossing midnight whatever the clock says here. `start_date`/`end_date`
# pin `daily` to the same two dates -- `forecast_days` and `start_hour` cannot be combined,
# the API answers 400.
today=$(python3 -c 'import datetime; print(datetime.date.today())')
tomorrow=$(python3 -c 'import datetime; print(datetime.date.today() + datetime.timedelta(days=1))')

# The variable lists are PLAN 4a's, verbatim. Keep those two in step; only the window here
# differs from what the device sends.
url="https://api.open-meteo.com/v1/forecast\
?latitude=53.3331&longitude=-6.2489\
&hourly=temperature_2m,apparent_temperature,precipitation,precipitation_probability,shortwave_radiation,cloud_cover,is_day,weather_code\
&daily=sunrise,sunset\
&start_hour=${today}T20:00&end_hour=${tomorrow}T08:00\
&start_date=${today}&end_date=${tomorrow}&timezone=auto"

echo "fetching $url"
curl -fsS "$url" -o "$out_json"
bytes=$(wc -c < "$out_json")
echo "recorded $bytes bytes -> $out_json"

if [[ $bytes -gt 8192 ]]; then
    echo "refusing: $bytes bytes is over WX_PARSE_MAX_BYTES, so the parser would reject" >&2
    echo "its own fixture. Either the query grew or the API changed shape." >&2
    exit 1
fi

python3 - "$out_json" "$out_h" <<'PY'
import sys, datetime

src, dst = sys.argv[1], sys.argv[2]
raw = open(src, encoding="utf-8").read().strip()

def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')

width = 88
chunks = [raw[i:i + width] for i in range(0, len(raw), width)]
body = "\n".join('    "%s"' % esc(c) for c in chunks)

print("""/* A real Open-Meteo response, recorded rather than fetched.
 *
 * Captured %s for Dublin city centre (53.3331, -6.2489) with PLAN 4a's variables and
 * an explicitly pinned window -- 20:00 local to 08:00 the next morning, which is the
 * thirteen hours the device would fetch at 20:00. %d bytes. A test that reaches the
 * network is not a test: it fails when the weather changes, when the DNS is slow, and on
 * an aeroplane, and none of those are the parser being wrong (PLAN 5b).
 *
 * Regenerate with tools/record_forecast.sh when the query in PLAN 4a changes. Do not
 * hand-edit: the point of a fixture is that it is a thing the server actually said.
 *
 * Properties this fixture is relied on for, asserted in test_wx_parse.c:
 *   - 13 hours in every series
 *   - it crosses local midnight, so a date rollover is covered
 *   - it contains a zero-precipitation hour and a zero-radiation night hour
 *   - daily carries two days of sunrise and sunset
 *
 * The window is pinned so that all four survive a re-recording made at any hour of the
 * day. That hourly.time[0] is the hour in progress under PLAN 4a's own query is a claim
 * about the API rather than about the parser, and is verified live in PLAN 4a.
 */
#pragma once

static const char FORECAST_DUBLIN[] =
%s;""" % (datetime.date.today().isoformat(), len(raw), body), file=open(dst, "w"))
PY

echo "wrote $out_h"
echo
echo "Now re-read the asserted values in test/host/test_wx_parse.c against the new JSON."
