* GIT — in-session record-git verb for Rocket UniData (Model B).
*
* Catalog this UniBasic program as the VOC verb GIT.  It drives the record loop
* natively on the CURRENT UniData session (OPEN/SELECT/READNEXT/READ) and calls
* the record-git engine's git-object work through CallC (GITINIT / GITSTAGE /
* GITCOMMIT / GITLOG, built into libu2callc.so from gitcallcb.c) — so there is
* no InterCall, no second session and no extra licence used, and C never calls
* back into UniBasic (no run-machine nesting).
*
* Build: see packages/git/src/gitcallcb.c and the cfuncdef/callbas.mk relink of
* libu2callc.so (memory: udt-git-verb-callc).  Requires UDT.OPTIONS 88 (CallC).
*
      SENT = @SENTENCE                 ;* capture BEFORE any PERFORM (which
      SUB  = FIELD(SENT, " ", 2)       ;*  overwrites @SENTENCE)
      FILE = FIELD(SENT, " ", 3)
      MSG  = "" ; NW = DCOUNT(SENT, " ")
      FOR I = 1 TO NW
         IF FIELD(SENT, " ", I) = "-m" THEN MSG = FIELD(SENT, " ", I + 1)
      NEXT I
*
      PERFORM "UDT.OPTIONS 88 ON"       ;* enable CallC
      REPO = @PATH : "/.git"            ;* the account's own repository
      OUT  = SPACE(1000000)
*
      BEGIN CASE
      CASE SUB = "INIT"
         R = CALLC GITINIT(REPO, OUT) ; GOSUB SHOW
      CASE SUB = "ADD"
         GOSUB ADDFILE ; PRINT NSTAGED : " record(s) staged"
      CASE SUB = "COMMIT"
         R = CALLC GITCOMMIT(REPO, MSG, OUT) ; GOSUB SHOW
      CASE SUB = "LOG"
         R = CALLC GITLOG(REPO, "20", OUT) ; GOSUB SHOW
      CASE 1
         PRINT "usage: GIT <init | add file | commit -m msg | log>"
      END CASE
      STOP
*
   ADDFILE:
      NSTAGED = 0
      OPEN FILE TO F ELSE PRINT "GIT: cannot open " : FILE ; RETURN
      SELECT F
      LOOP WHILE READNEXT ID DO
         READ REC FROM F, ID THEN
            OUT = SPACE(200)
            R = CALLC GITSTAGE(REPO, FILE, ID, REC, OUT)
            NSTAGED += 1
         END
      REPEAT
      RETURN
*
   SHOW:
      R = TRIMB(R) ; NL = DCOUNT(R, @AM)
      FOR I = 1 TO NL ; PRINT R<I> ; NEXT I
      RETURN
