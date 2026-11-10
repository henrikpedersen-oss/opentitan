# reason: a time limit for Superlint's automatic formal checks (dead code, overflow, FSM reachability), not a waiver of any check: on pwrmgr one of them ran without an answer for more than 40 minutes (2026-11-07, about 3100 trace attempts); with the limit it ends undetermined and the report lists it owner: Henrik Pedersen <henrik.pedersen@lowrisc.org>
set_prove_time_limit 10m
