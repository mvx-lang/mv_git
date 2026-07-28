* GIT — in-session record-git verb for Rocket UniData (Model B).
*
* Catalog this UniBasic program as the VOC verb GIT.  It drives the record loop
* natively on the CURRENT UniData session (OPEN/SELECT/READNEXT/READ) and calls
* the record-git engine's git-object work through CallC (GITINIT / GITSTAGE /
* GITSTAGEBLOB / GITCOMMIT / GITLOG, built into libu2callc.so from gitcallcb.c)
* — no InterCall, no second session, no extra licence used, and C never calls
* back into UniBasic (no run-machine nesting).  git-object output is returned
* through <repo>/gitmsg (CallC string return does not marshal), OSREAD in SHOW.
*
* Commands: INIT | ADD file | ADD -A [-o] | COMMIT -m msg | LOG
*   ADD -A stages the whole account (every local file's data + dictionary).
*   -o (or --open) writes the portable OPEN account format: a synthesised
*   <file>.DICT/%FILE% (DIR/hash) per file, the .mv-account descriptor, and it
*   drops the system VOC items (verbs V, keywords K, and the platform file/Q
*   pointers) so a foreign account's verbs never shadow the destination's.
*
      SENT = @SENTENCE                 ;* capture BEFORE any PERFORM (overwrites)
      SUB  = FIELD(SENT, " ", 2)
      ARG3 = FIELD(SENT, " ", 3)
      MSG  = "" ; NW = DCOUNT(SENT, " ")
      FOR I = 1 TO NW
         IF FIELD(SENT, " ", I) = "-m" THEN MSG = FIELD(SENT, " ", I + 1)
      NEXT I
      OPENFMT = 0
      IF INDEX(SENT, " -o", 1) # 0 OR INDEX(SENT, " --open", 1) # 0 THEN OPENFMT = 1
*
      PERFORM "UDT.OPTIONS 88 ON"       ;* enable CallC
      REPO = @PATH : "/.git"            ;* the account's own repository
      AM = CHAR(254) ; VM = CHAR(253) ; LF = CHAR(10)
*
      BEGIN CASE
      CASE SUB = "INIT"
         R = CALLC GITINIT(REPO) ; GOSUB SHOW
      CASE SUB = "ADD" AND (ARG3 = "-A" OR ARG3 = "." OR ARG3 = "--all")
         GOSUB ADDALL
         PRINT NSTAGED : " record(s) staged across " : NFILES : " file(s)"
      CASE SUB = "ADD"
         FILE = ARG3 ; ISVOC = 0 ; NSTAGED = 0 ; GOSUB STAGEDATA
         PRINT NSTAGED : " record(s) staged"
      CASE SUB = "COMMIT"
         R = CALLC GITCOMMIT(REPO, MSG) ; GOSUB SHOW
      CASE SUB = "LOG"
         R = CALLC GITLOG(REPO, "20") ; GOSUB SHOW
      CASE SUB = "CHECKOUT" OR SUB = "MATERIALIZE"
         GOSUB CHECKOUT
         PRINT NREST : " record(s) restored across " : NFC : " file(s)"
      CASE 1
         PRINT "usage: GIT <init | add file | add -A [-o] | commit -m msg | log | checkout>"
      END CASE
      STOP
*
*  Stage the whole account: collect the local files from VOC first (draining the
*  VOC select before opening any file — nested selects on the default list would
*  clobber each other), then stage each file's data + dictionary (+ open-form
*  controls).
   ADDALL:
      NSTAGED = 0 ; NFILES = 0 ; FILES = ""
      OPEN "VOC" TO VOCF ELSE PRINT "GIT: no VOC" ; RETURN
      SELECT VOCF
      LOOP WHILE READNEXT VID DO
         READ VREC FROM VOCF, VID THEN
            VT = VREC<1>
            IF VT = "F" OR VT = "DIR" THEN
               DP = VREC<2> ; DD = VREC<3> ; LOC = 1
               IF INDEX(DP,"/",1) OR DP[1,1] = "@" THEN LOC = 0
               IF INDEX(DD,"/",1) OR DD[1,1] = "@" THEN LOC = 0
               L = LEN(VID)
               IF VID[1,1]="_" AND VID[L,1]="_" THEN LOC = 0
               IF VID[1,1]="&" AND VID[L,1]="&" THEN LOC = 0
               IF LOC THEN FILES<-1> = VID : VM : VT
            END
         END
      REPEAT
      NF = DCOUNT(FILES, AM)
      FOR K = 1 TO NF
         FILE  = FIELD(FILES<K>, VM, 1)
         FTYPE = FIELD(FILES<K>, VM, 2)
         ISVOC = (FILE = "VOC")
         GOSUB STAGEDATA
         GOSUB STAGEDICT
         IF OPENFMT THEN
            IF FTYPE = "DIR" THEN CTL = "DIR" ELSE CTL = "hash"
            R = CALLC GITSTAGEBLOB(REPO, FILE:".DICT/%FILE%", CTL)
         END
         NFILES += 1
      NEXT K
      IF OPENFMT THEN
         NM = FIELD(@PATH, "/", DCOUNT(@PATH, "/"))
         DESC = "# MV account descriptor":LF:"name = ":NM:LF:"version = 1":LF:"hash = lmdb":LF
         R = CALLC GITSTAGEBLOB(REPO, ".mv-account", DESC)
      END
      RETURN
*
*  Stage FILE's data records.  When staging the master VOC in the open form,
*  drop the auto-created system items the destination supplies its own of.
   STAGEDATA:
      OPEN FILE TO DF ELSE PRINT "GIT: cannot open ":FILE ; RETURN
      SELECT DF
      LOOP WHILE READNEXT RID DO
         READ RREC FROM DF, RID THEN
            DROP = 0
            IF OPENFMT AND ISVOC THEN
               RT = RREC<1>
               IF RT="V" OR RT="K" THEN DROP = 1
               IF RT="F" OR RT="LF" OR RT="DF" OR RT="DIR" THEN DROP = 1
               IF RT="Q" OR RT="X" OR RT="R" THEN DROP = 1
            END
            IF NOT(DROP) THEN
               OUT = SPACE(20)
               R = CALLC GITSTAGE(REPO, FILE, RID, RREC)
               NSTAGED += 1
            END
         END
      REPEAT
      RETURN
*
*  Stage FILE's dictionary records.
   STAGEDICT:
      OPEN "DICT", FILE TO DCF ELSE RETURN
      SELECT DCF
      LOOP WHILE READNEXT RID DO
         READ RREC FROM DCF, RID THEN

            R = CALLC GITSTAGE(REPO, FILE:".DICT", RID, RREC)
            NSTAGED += 1
         END
      REPEAT
      RETURN
*
*  Materialise the git HEAD into this UniData account: CREATE.FILE each file
*  from its open-form %FILE% control (DIR/hash), then WRITE every record back on
*  the current session.  C (GITFILES/GITCAT) reads the git objects; the writes
*  are native, so no InterCall and no extra licence.
   CHECKOUT:
      NREST = 0 ; NFC = 0 ; FCTL = ".DICT/%FILE%" ; LC = LEN(FCTL)
      R = CALLC GITFILES(REPO)
      OSREAD PATHS FROM REPO : "/gitmsg" ELSE PATHS = ""
      NP = DCOUNT(PATHS, AM)
*     pass 1 - create each file from its %FILE% control (open format)
      FOR I = 1 TO NP
         P = PATHS<I> ; LP = LEN(P)
         IF LP > LC AND P[LP-LC+1, LC] = FCTL THEN
            BASE = P[1, LP-LC]
            R = CALLC GITCAT(REPO, P)
            OSREAD TY FROM REPO : "/gitcat" ELSE TY = ""
            TY = TRIM(TY)
            IF TY = "DIR" THEN
               EXECUTE "CREATE.FILE ":BASE:" DIRECTORY" CAPTURING JUNK
            END ELSE
               EXECUTE "CREATE.FILE ":BASE:" DYNAMIC" CAPTURING JUNK
            END
            NFC += 1
         END
      NEXT I
*     pass 2 - WRITE every record (skip the descriptor and the %FILE% controls)
      FOR I = 1 TO NP
         P = PATHS<I> ; LP = LEN(P)
         IF P = ".mv-account" OR P = ".udt" OR P = ".mvx" THEN GOTO NEXTREC
         IF LP > LC AND P[LP-LC+1, LC] = FCTL THEN GOTO NEXTREC
         SL = INDEX(P, "/", 1)
         IF SL = 0 THEN GOTO NEXTREC
         FN = P[1, SL-1] ; ID = P[SL+1, LP]
         R = CALLC GITCAT(REPO, P)
         OSREAD REC FROM REPO : "/gitcat" ELSE REC = ""
         LFN = LEN(FN)
         IF LFN > 5 AND FN[LFN-4, 5] = ".DICT" THEN
            OPEN "DICT", FN[1, LFN-5] TO FV ELSE GOTO NEXTREC
         END ELSE
            OPEN FN TO FV ELSE GOTO NEXTREC
         END
         WRITE REC ON FV, ID
         NREST += 1
   NEXTREC:
      NEXT I
      RETURN
*
*  The git-object output comes back through <repo>/gitmsg (CallC's string
*  return does not marshal reliably), so OSREAD it and print the @AM lines.
   SHOW:
      OSREAD MSGT FROM REPO : "/gitmsg" ELSE MSGT = ""
      IF MSGT # "" THEN
         NL = DCOUNT(MSGT, AM)
         FOR I = 1 TO NL ; PRINT MSGT<I> ; NEXT I
      END
      RETURN
