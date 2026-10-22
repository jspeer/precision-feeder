// The golden servo trace: what firmware 2.0.2 (before the mechanism interface) did in servo_session()
// (test_core.cpp): every servo command, release and tray-state save, with its time in milliseconds since boot.
// Recorded from 2.0.2 on 2026-10-21. Record again only for an intended behaviour change (see test_core.cpp).
// Recorded again for 2.1.2: a reload, a declare and the end of calibration now save their move as an intent (to=<slot>)
// and clear it once settled (Codex review). Every servo command and release, with its time, is unchanged.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

static const char SERVO_GOLDEN[] = R"golden(0 servo 2.500
1000 release
1000 state pos=0 to=255 unknown=0 flush=0
1500 state pos=0 to=1 unknown=0 flush=1
1500 servo 27.500
1500 feed 1 -> ok
2500 servo 2.500
3000 calibration 0 0 0 0 0 0 0 pulse 544-2400
3500 servo 27.500
4500 state pos=1 to=255 unknown=0 flush=1
5500 release
8500 calibration 0 0 0 0 0 0 0 pulse 544-2400
67500 state pos=1 to=2 unknown=0 flush=1
67500 servo 52.500
67500 feed 3 -> ok
68500 servo 27.500
69500 servo 52.500
70500 state pos=2 to=255 unknown=0 flush=1
75500 state pos=2 to=3 unknown=0 flush=1
75500 servo 77.500
76500 servo 52.500
77500 servo 77.500
78500 state pos=3 to=255 unknown=0 flush=1
83500 state pos=3 to=4 unknown=0 flush=1
83500 servo 102.500
84500 servo 77.500
85500 servo 102.500
86500 state pos=4 to=255 unknown=0 flush=1
87500 release
108500 state pos=4 to=5 unknown=0 flush=1
108500 servo 127.500
108500 feed 1 no wiggle -> ok
109500 state pos=5 to=255 unknown=0 flush=1
110500 release
111500 state pos=0 to=0 unknown=0 flush=1
111500 servo 2.500
111500 reload
112500 state pos=0 to=255 unknown=0 flush=1
112500 release
113500 servo 2.500
113500 calibration on
113500 servo 52.500
113500 servo 54.000
113500 servo 76.800
113500 jog 3 -7 -> ok
113500 servo 76.800
113500 pulse 600-2300 -> ok
113500 feed while calibrating -> calibrating
114500 calibration 0 0 15 -7 0 0 0 pulse 600-2300
114500 state pos=0 to=0 unknown=0 flush=1
114500 servo 2.500
114500 calibration off
115500 state pos=0 to=255 unknown=0 flush=1
115500 release
126500 state pos=0 to=255 unknown=0 flush=1
126500 state pos=0 to=1 unknown=0 flush=1
126500 servo 27.500
126500 feed 1 then stop -> ok
127000 state pos=0 to=255 unknown=1 flush=1
127000 release
129000 feed while unknown -> position unknown
129000 state pos=2 to=2 unknown=0 flush=1
129000 servo 54.000
129000 declared 2
130000 state pos=2 to=255 unknown=0 flush=1
130000 release
140000 state pos=2 to=255 unknown=0 flush=1
140000 state pos=2 to=3 unknown=0 flush=1
140000 servo 76.800
140000 feed 2 then stop in gap -> ok
141000 servo 54.000
142000 servo 76.800
143000 state pos=3 to=255 unknown=0 flush=1
144000 state pos=3 to=255 unknown=0 flush=1
145000 release
156000 state pos=3 to=255 unknown=0 flush=1
156000 state pos=3 to=4 unknown=0 flush=1
156000 servo 102.500
156000 feed 1 then power cut -> ok
157000 servo 76.800
157250 power cut
0 state pos=3 to=255 unknown=1 flush=1
1000 state pos=3 to=255 unknown=1 flush=0
1500 state pos=0 to=0 unknown=0 flush=1
1500 servo 2.500
1500 button (reload)
2500 state pos=0 to=255 unknown=0 flush=1
2500 release
5500 calibration 0 0 15 -7 0 0 0 pulse 600-2300
601000 state pos=0 to=255 unknown=0 flush=0
602000 state pos=0 to=1 unknown=0 flush=1
602000 servo 27.500
602000 state pos=0 to=1 unknown=0 flush=0
603000 servo 2.500
604000 servo 27.500
605000 state pos=1 to=255 unknown=0 flush=1
610000 state pos=1 to=2 unknown=0 flush=1
610000 servo 54.000
611000 servo 27.500
612000 servo 54.000
613000 state pos=2 to=255 unknown=0 flush=1
614000 release
723500 after the 08:10 event
723500 state pos=5 to=5 unknown=0 flush=1
723500 servo 127.500
724500 state pos=5 to=255 unknown=0 flush=1
724500 release
734500 state pos=5 to=255 unknown=0 flush=1
734500 state pos=5 to=6 unknown=0 flush=1
734500 servo 152.500
734500 feed last -> ok
735500 servo 127.500
736500 servo 152.500
737500 state pos=6 to=255 unknown=0 flush=1
738500 release
745500 feed empty -> tray empty
745500 state pos=0 to=0 unknown=0 flush=1
745500 servo 2.500
745500 button (empty: reload)
746500 state pos=0 to=255 unknown=0 flush=1
746500 release
)golden";
