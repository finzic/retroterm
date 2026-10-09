  10 REM RetroSSH terminal - ZX Spectrum 48K + Interface 1, or 128K/+2 RS232
  20 REM STOP (SYMBOL SHIFT+A) quits. TRUE VIDEO = Ctrl prefix
  30 REM EDIT = ESC, DELETE = DEL, cursor keys = arrows
  35 REM GRAPHICS (CAPS SHIFT+9) = WiFi setup
  40 LET b=1200
  50 INPUT "Port: 1=Interface 1, 2=128K/+2 RS232 ";p
  60 IF p<>1 AND p<>2 THEN GO TO 50
  70 IF p=1 THEN GO SUB 9100
  80 IF p=2 THEN GO SUB 9200
  90 POKE 23658,0: BORDER 0: PAPER 0: INK 4: CLS
  95 LET ctl=0: LET l$="": PRINT "RetroSSH - STOP quits": PRINT "GRAPHICS = WiFi setup"
 100 LET k$=INKEY$
 110 IF k$<>"" AND k$<>l$ THEN GO SUB 500
 120 LET l$=k$
 130 LET r$=INKEY$#s
 140 IF r$<>"" THEN GO SUB 300
 150 GO TO 100
 300 REM --- received character ---
 310 LET c=CODE r$
 320 IF c>31 AND c<127 THEN PRINT r$;: RETURN
 330 IF c=10 THEN POKE 23692,255: PRINT : RETURN
 340 IF c=8 THEN PRINT CHR$ 8;: RETURN
 350 IF c=7 THEN BEEP .02,24
 360 RETURN
 500 REM --- key pressed ---
 510 LET k=CODE k$
 520 IF k=226 THEN GO TO 900
 525 IF k=15 THEN GO SUB 8000: RETURN
 530 IF k=4 THEN LET ctl=1: RETURN
 540 IF ctl=0 THEN GO TO 580
 550 LET ctl=0
 560 IF k>96 AND k<123 THEN LET k=k-96
 570 IF k>64 AND k<91 THEN LET k=k-64
 580 IF k=12 THEN LET k=127
 590 IF k=7 THEN LET k=27
 600 IF k>7 AND k<12 THEN PRINT #s;CHR$ 27;"[";"DCBA"(k-7);: RETURN
 610 PRINT #s;CHR$ k;
 620 RETURN
 900 IF s=4 THEN CLOSE #4
 910 STOP
8000 REM --- WiFi setup ---
8010 INPUT "SSID: "; LINE w$
8020 IF w$="" THEN RETURN
8030 INPUT "Passphrase: "; LINE q$
8040 INPUT "RTS/CTS wired (y/n)? "; LINE f$
8050 PRINT "Configuring modem..."
8055 REM +++ with guard times reaches command mode even if a session is open
8060 GO SUB 8700: LET a$="+++": GO SUB 8600: GO SUB 8700: LET a$="": GO SUB 8650
8070 LET e$=w$: GO SUB 8500: LET a$="AT+CWJAP="+e$
8080 LET e$=q$: GO SUB 8500: LET a$=a$+","+e$: GO SUB 8650
8090 LET a$="AT+TERM=dumb": GO SUB 8650
8100 LET a$="AT+FILTER=1": GO SUB 8650
8110 LET a$="AT+WIN=32,22": GO SUB 8650
8120 IF f$="y" OR f$="Y" THEN LET a$="AT&K3": GO SUB 8650
8130 LET a$="AT&W": GO SUB 8650
8140 PRINT "Sent. Joining takes up to 20s": PRINT "Check with AT+CWJAP?"
8150 RETURN
8500 REM --- quote e$, escaping " and \ ---
8510 LET t$=CHR$ 34
8520 FOR i=1 TO LEN e$
8530 IF e$(i)=CHR$ 34 OR e$(i)="\" THEN LET t$=t$+"\"
8540 LET t$=t$+e$(i): NEXT i
8550 LET e$=t$+CHR$ 34: RETURN
8600 PRINT #s;a$;: RETURN
8650 PRINT #s;a$;CHR$ 13;: RETURN
8700 REM --- wait 1.2 s (60 frames) ---
8710 LET t0=PEEK 23672+256*PEEK 23673
8720 LET d=PEEK 23672+256*PEEK 23673-t0
8730 IF d<0 THEN LET d=d+65536
8740 IF d<60 THEN GO TO 8720
8750 RETURN
9000 REM Type only the block for your port: the editor may reject the other
9100 REM --- Interface 1 (48K or 128K/+2 with IF1 attached) ---
9110 FORMAT "b";b: OPEN #4;"b": LET s=4: RETURN
9200 REM --- 128K/+2 RS232/MIDI socket, 128 BASIC only ---
9210 FORMAT LINE b: FORMAT LPRINT "r": LET s=3: RETURN
