/* A real Open-Meteo response, recorded rather than fetched.
 *
 * Captured 2026-09-03 for Dublin city centre (53.3331, -6.2489) with PLAN 4a's variables and
 * an explicitly pinned window -- 20:00 local to 08:00 the next morning, which is the
 * thirteen hours the device would fetch at 20:00. 1449 bytes. A test that reaches the
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
    "{\"latitude\":53.329914,\"longitude\":-6.2360687,\"generationtime_ms\":0.1589059829711914,\"utc"
    "_offset_seconds\":3600,\"timezone\":\"Europe/Dublin\",\"timezone_abbreviation\":\"GMT+1\",\"elevat"
    "ion\":14.0,\"hourly_units\":{\"time\":\"iso8601\",\"temperature_2m\":\"°C\",\"apparent_temperature\":"
    "\"°C\",\"precipitation\":\"mm\",\"precipitation_probability\":\"%\",\"shortwave_radiation\":\"W/m²\",\""
    "cloud_cover\":\"%\",\"is_day\":\"\",\"weather_code\":\"wmo code\"},\"hourly\":{\"time\":[\"2026-09-03T20"
    ":00\",\"2026-09-03T21:00\",\"2026-09-03T22:00\",\"2026-09-03T23:00\",\"2026-09-04T00:00\",\"2026-0"
    "9-04T01:00\",\"2026-09-04T02:00\",\"2026-09-04T03:00\",\"2026-09-04T04:00\",\"2026-09-04T05:00\","
    "\"2026-09-04T06:00\",\"2026-09-04T07:00\",\"2026-09-04T08:00\"],\"temperature_2m\":[15.7,15.4,15"
    ".6,15.5,15.2,15.2,15.2,15.2,15.3,15.2,14.9,14.6,14.7],\"apparent_temperature\":[14.4,14.9,"
    "15.2,15.2,15.5,14.9,14.7,14.5,14.1,13.9,12.8,11.8,11.2],\"precipitation\":[0.70,0.70,0.50,"
    "0.60,1.10,0.30,0.00,0.00,0.00,0.00,0.00,0.00,0.00],\"precipitation_probability\":[95,98,10"
    "0,100,98,92,78,60,43,27,12,2,0],\"shortwave_radiation\":[14.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,"
    "0.0,0.0,0.0,1.0,54.0],\"cloud_cover\":[100,100,100,100,100,100,100,100,100,100,100,100,100"
    "],\"is_day\":[1,0,0,0,0,0,0,0,0,0,0,1,1],\"weather_code\":[53,53,53,53,55,51,3,3,3,3,3,3,3]}"
    ",\"daily_units\":{\"time\":\"iso8601\",\"sunrise\":\"iso8601\",\"sunset\":\"iso8601\"},\"daily\":{\"time\""
    ":[\"2026-09-03\",\"2026-09-04\"],\"sunrise\":[\"2026-09-03T06:38\",\"2026-09-04T06:39\"],\"sunset\":"
    "[\"2026-09-03T20:10\",\"2026-09-04T20:08\"]}}";
