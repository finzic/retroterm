   10 REM RetroSSH terminal - BBC Micro Model B, BASIC II
   20 REM Uses the RS423 port. f0 quits, f1 sets up WiFi.
   30 REM Rate for *FX7/*FX8: 4=1200 5=2400 6=4800 7=9600
   40 B%=4
   50 MODE 3
   60 OSCLI "FX7,"+STR$B%
   70 OSCLI "FX8,"+STR$B%
   80 *FX2,2
   90 *FX4,1
  100 *FX225,128
  110 *FX229,1
  120 *FX21,1
  130 PRINT "RetroSSH terminal - f0 quits, f1 WiFi setup"
  140 REPEAT
  150   REPEAT
  160     A%=145:X%=1:R%=USR&FFF4
  170     E%=R% AND &1000000
  180     IF E%=0 THEN PROCrx((R% AND &FF0000) DIV &10000)
  190   UNTIL E%
  200   K%=INKEY(0)
  210   IF K%>=0 THEN PROCkey(K%)
  220 UNTIL K%=128
  230 *FX3,0
  240 *FX4,0
  250 *FX225,1
  260 *FX229,0
  270 *FX2,0
  280 END
  290 :
  300 DEF PROCrx(C%)
  310 IF C%>31 AND C%<127 THEN VDU C%:ENDPROC
  320 IF C%=13 OR C%=10 OR C%=8 OR C%=7 THEN VDU C%:ENDPROC
  330 IF C%=9 THEN PRINT SPC(8-(POS MOD 8));
  340 ENDPROC
  350 :
  400 DEF PROCkey(C%)
  405 IF C%=129 THEN PROCsetup:ENDPROC
  410 IF C%>=136 AND C%<=139 THEN PROCsend(27):PROCsend(91):PROCsend(ASC(MID$("DCBA",C%-135,1))):ENDPROC
  420 IF C%<128 THEN PROCsend(C%)
  430 ENDPROC
  440 :
  500 DEF PROCsend(C%)
  510 *FX3,7
  520 VDU C%
  530 *FX3,0
  540 ENDPROC
  550 :
  600 DEF PROCsetup
  610 LOCAL S$,P$,F$
  620 PRINT:INPUT LINE "SSID: " S$
  630 IF S$="" THEN ENDPROC
  640 INPUT LINE "Passphrase: " P$
  650 INPUT LINE "RTS/CTS wired (Y/N)? " F$
  660 PRINT "Configuring modem..."
  670 REM +++ with guard times reaches command mode even if a session is open
  680 PROCwait:PROCsends("+++"):PROCwait:PROCsends(CHR$13)
  690 PROCsends("AT+CWJAP="+FNq(S$)+","+FNq(P$)+CHR$13)
  700 PROCsends("AT+TERM=dumb"+CHR$13)
  710 PROCsends("AT+FILTER=1"+CHR$13)
  720 PROCsends("AT+WIN=80,25"+CHR$13)
  730 IF F$="Y" OR F$="y" THEN PROCsends("AT&K3"+CHR$13)
  740 PROCsends("AT&W"+CHR$13)
  750 PRINT "Sent. Joining takes up to 20s: check with AT+CWJAP?"
  760 ENDPROC
  770 :
  800 DEF PROCwait
  810 LOCAL T%:T%=TIME+120
  820 REPEAT UNTIL TIME>=T%
  830 ENDPROC
  840 :
  850 DEF PROCsends(A$)
  860 LOCAL I%
  870 FOR I%=1 TO LEN A$:PROCsend(ASC MID$(A$,I%,1)):NEXT
  880 ENDPROC
  890 :
  900 DEF FNq(A$)
  910 LOCAL I%,C$,R$
  920 R$=CHR$34
  930 FOR I%=1 TO LEN A$
  940 C$=MID$(A$,I%,1):IF C$=CHR$34 OR C$="\" THEN R$=R$+"\"
  950 R$=R$+C$:NEXT
  960 =R$+CHR$34
