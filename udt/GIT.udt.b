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
* Commands: INIT | ADD file | ADD -A [-o] | COMMIT -m msg | LOG | STATUS |
*           DIFF [file] | BRANCH [name] | CHECKOUT
*   ADD -A stages the whole account (every local file's data + dictionary).
*   -o (or --open) writes the portable OPEN account format: a synthesised
*   <file>.DICT/%FILE% (DIR/hash) per file, the .mv-account descriptor, and it
*   drops the system VOC items (verbs V, keywords K, and the platform file/Q
*   pointers) so a foreign account's verbs never shadow the destination's.
*   STATUS and DIFF compare the live records against HEAD entirely on the
*   current session (records read natively, git objects via CallC GITFILES /
*   GITCAT) — no InterCall, no second session.  BRANCH is a pure git op.
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
         SKIPOBJ = 1                     ;* -A skips compiled objects by default
         GOSUB ADDALL
         PRINT NSTAGED : " record(s) staged across " : NFILES : " file(s)"
      CASE SUB = "ADD"
         FILE = ARG3 ; ISVOC = 0 ; NSTAGED = 0 ; SKIPOBJ = 0  ;* explicit: all
         GOSUB STAGEDATA
         PRINT NSTAGED : " record(s) staged"
      CASE SUB = "COMMIT"
         R = CALLC GITCOMMIT(REPO, MSG) ; GOSUB SHOW
      CASE SUB = "LOG"
         R = CALLC GITLOG(REPO, "20") ; GOSUB SHOW
      CASE SUB = "CHECKOUT" OR SUB = "MATERIALIZE"
         GOSUB CHECKOUT
         PRINT NREST : " record(s) restored across " : NFC : " file(s)"
         GOSUB IXPROMPT
      CASE SUB = "STATUS"
         GOSUB STATUS
      CASE SUB = "DIFF"
         DFILE = ARG3 ; GOSUB DIFF
      CASE SUB = "BRANCH"
         R = CALLC GITBRANCH(REPO, ARG3) ; GOSUB SHOW
      CASE 1
         PRINT "usage: GIT <init | add file | add -A [-o] | commit -m msg |"
         PRINT "            log | status | diff [file] | branch [name] | checkout>"
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
            GOSUB FILECLASS       ;* CTL = DIR | hash <modulo> DYNAMIC|STATIC
            R = CALLC GITSTAGEBLOB(REPO, FILE:".DICT/%FILE%", CTL)
            GOSUB IXSTAGE         ;* synthesise %INDEXES% from the live indexes
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
*  Open-form class of FILE for its %FILE% control: "DIR" for a directory file,
*  else "hash <modulo> DYNAMIC|STATIC" carrying the live file's real geometry so
*  a clone recreates it at true size (and keeps a static file static).  FILEINFO
*  in-session is reliable: key 3 = type (2 static hash, 3 dynamic, else dir), key
*  5 = current modulus.  A directory file has no modulus (0); a dynamic hashed
*  file (also a directory on disk) has a positive one — the modulus separates
*  them, mirroring udt-git's mv_fileclass.
   FILECLASS:
      CTL = "hash"
      OPEN FILE TO FCVAR THEN
         FTY = FILEINFO(FCVAR, 3) ; MODL = FILEINFO(FCVAR, 5)
         IF MODL <= 0 THEN
            CTL = "DIR"
         END ELSE
            IF FTY = 2 THEN DS = "STATIC" ELSE DS = "DYNAMIC"
            CTL = "hash " : MODL : " " : DS
         END
      END
      RETURN
*
*  Synthesise <file>.DICT/%INDEXES% from the file's live alternate-key indexes so
*  they travel (#10).  INDICES() gives the @AM name list — exactly the portable
*  form MVX keeps as a dict record; fold marks to newlines (as every record blob
*  is stored) before staging, and only stage when the file has indexes.
   IXSTAGE:
      OPEN FILE TO IXVAR THEN
         IXL = INDICES(IXVAR)
         IF IXL # "" THEN
            IXG = IXL ; CONVERT AM TO LF IN IXG
            R = CALLC GITSTAGEBLOB(REPO, FILE:".DICT/%INDEXES%", IXG)
         END
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
*           A compiled BASIC object (binary, holds NUL bytes) is derived from its
*           source and rebuilt on the target by CATALOG/BUILD — never stage one on
*           a blanket ADD -A; an explicit ADD <file> (SKIPOBJ = 0) still takes it.
*           (It also cannot survive CallC's strlen marshalling, so this keeps the
*           commit whole rather than truncated.)
            IF SKIPOBJ AND INDEX(RREC, CHAR(0), 1) > 0 THEN DROP = 1
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
      IXCTL = ".DICT/%INDEXES%" ; LIC = LEN(IXCTL)
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
         IF LP > LIC AND P[LP-LIC+1, LIC] = IXCTL THEN GOTO NEXTREC
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
*     pass 3 - DETECT index changes only (#11): a file's indexes changed when the
*     set declared in its %INDEXES% control differs from the set live now.  Do not
*     rebuild here — the caller reports the changes and asks the user first.
      IXWANT = ""
      FOR I = 1 TO NP
         P = PATHS<I> ; LP = LEN(P)
         IF LP > LIC AND P[LP-LIC+1, LIC] = IXCTL THEN
            BASE = P[1, LP-LIC]
            R = CALLC GITCAT(REPO, P)
            OSREAD IXL FROM REPO : "/gitcat" ELSE IXL = ""
            LIVE = "" ; OPEN BASE TO IXOV THEN LIVE = INDICES(IXOV)
            CHG = 0 ; DW = AM:IXL:AM ; LW = AM:LIVE:AM
            FOR J = 1 TO DCOUNT(IXL, AM)
               IF INDEX(LW, AM:TRIM(IXL<J>):AM, 1) = 0 THEN CHG = 1
            NEXT J
            FOR J = 1 TO DCOUNT(LIVE, AM)
               IF INDEX(DW, AM:TRIM(LIVE<J>):AM, 1) = 0 THEN CHG = 1
            NEXT J
            IF CHG THEN IXWANT<-1> = BASE
         END
      NEXT I
      RETURN
*
*  If any file's indexes changed, list them and ask before rebuilding (#11).
   IXPROMPT:
      NW2 = DCOUNT(IXWANT, AM)
      IF NW2 = 0 THEN RETURN
      PRINT "Indexes have changed for " : NW2 : " file(s):"
      FOR I = 1 TO NW2
         BASE = IXWANT<I>
         R = CALLC GITCAT(REPO, BASE:IXCTL)
         OSREAD IXL FROM REPO : "/gitcat" ELSE IXL = ""
         PRINT "  " : BASE : " (" : CONVERT(AM, " ", TRIM(IXL)) : ")"
      NEXT I
      PRINT "Rebuild them now? (Y/N) " :
      INPUT ANS
      IF UPCASE(ANS)[1,1] # "Y" THEN
         PRINT "Indexes not rebuilt; run GIT CHECKOUT again to rebuild."
         RETURN
      END
      NIX = 0
      FOR I = 1 TO NW2
         BASE = IXWANT<I>
         R = CALLC GITCAT(REPO, BASE:IXCTL)
         OSREAD IXL FROM REPO : "/gitcat" ELSE IXL = ""
         NF2 = DCOUNT(IXL, AM)
         FOR J = 1 TO NF2
            FLD = TRIM(IXL<J>)
            IF FLD # "" THEN
               DATA ""
               EXECUTE "CREATE.INDEX ":BASE:" ":FLD CAPTURING JUNK
               NIX += 1
            END
         NEXT J
         IF NF2 > 0 THEN EXECUTE "BUILD.INDEX ":BASE:" ALL" CAPTURING JUNK
      NEXT I
      PRINT NIX : " index(es) rebuilt"
      RETURN
*
*  STATUS — compare the live records against HEAD on the current session.
*  GITFILES lists HEAD's blob paths; for each, GITCAT gives the committed record
*  and a native READ gives the working one — both carry the same marks, so a
*  plain string compare classifies modified (M) and deleted (D).  The %FILE%
*  control compares by CLASS only (the modulo is a sticky default, mvx#8).  New
*  (untracked) records are not enumerated here — the master VOC in the open form
*  intentionally omits the system items, so reporting them as untracked would be
*  noise; the standalone udt-git covers untracked.
   STATUS:
      FCTL = ".DICT/%FILE%" ; LC = LEN(FCTL) ; NCH = 0
      R = CALLC GITFILES(REPO)
      OSREAD PATHS FROM REPO : "/gitmsg" ELSE PATHS = ""
      NP = DCOUNT(PATHS, AM)
      FOR I = 1 TO NP
         P = PATHS<I> ; LP = LEN(P)
         BEGIN CASE
         CASE P = ".mv-account" OR P = ".udt" OR P = ".mvx"
            NULL                              ;* descriptor — left to standalone
         CASE LP > LC AND P[LP-LC+1, LC] = FCTL
            BASE = P[1, LP-LC] ; GOSUB CTLSTAT
            IF CHG THEN PRINT " M " : P ; NCH += 1
         CASE 1
            SL = INDEX(P, "/", 1)
            IF SL > 0 THEN
               FN = P[1, SL-1] ; RID = P[SL+1, LP]
               R = CALLC GITCAT(REPO, P)
               OSREAD CB FROM REPO : "/gitcat" ELSE CB = ""
               GOSUB OPENREC
               IF ROK THEN
                  READ LV FROM RV, RID THEN
                     GOSUB SAMEREC        ;* SAME = live matches committed
                     IF NOT(SAME) THEN PRINT " M " : P ; NCH += 1
                  END ELSE
                     PRINT " D " : P ; NCH += 1
                  END
               END ELSE
                  PRINT " D " : P ; NCH += 1
               END
            END
         END CASE
      NEXT I
      IF NCH = 0 THEN PRINT "nothing to commit, working tree clean"
      RETURN
*
*  DIFF [file] — for each committed record that differs from the live one, print
*  the changed attributes (-committed / +working).  Same session-only compare as
*  STATUS; an optional file name limits it.
   DIFF:
      FCTL = ".DICT/%FILE%" ; LC = LEN(FCTL) ; NDF = 0
      R = CALLC GITFILES(REPO)
      OSREAD PATHS FROM REPO : "/gitmsg" ELSE PATHS = ""
      NP = DCOUNT(PATHS, AM)
      FOR I = 1 TO NP
         P = PATHS<I> ; LP = LEN(P)
         IF P = ".mv-account" OR P = ".udt" OR P = ".mvx" THEN GOTO NEXTD
         IF LP > LC AND P[LP-LC+1, LC] = FCTL THEN GOTO NEXTD
         SL = INDEX(P, "/", 1) ; IF SL = 0 THEN GOTO NEXTD
         FN = P[1, SL-1] ; RID = P[SL+1, LP]
         IF DFILE # "" AND FN # DFILE THEN GOTO NEXTD
         R = CALLC GITCAT(REPO, P)
         OSREAD CB FROM REPO : "/gitcat" ELSE CB = ""
         GOSUB OPENREC ; LV = ""
         IF ROK THEN
            READ LV FROM RV, RID ELSE LV = ""
         END
         GOSUB SAMEREC
         IF NOT(SAME) THEN
            PRINT "diff " : P
            NAT = DCOUNT(CB, AM) ; NBT = DCOUNT(LV, AM)
            MX = NAT ; IF NBT > MX THEN MX = NBT
            FOR A = 1 TO MX
               OA = CB<A> ; NW = LV<A>
               IF OA # NW THEN
                  IF OA # "" THEN PRINT "-" : OA
                  IF NW # "" THEN PRINT "+" : NW
               END
            NEXT A
            NDF += 1
         END
      NEXTD:
      NEXT I
      IF NDF = 0 THEN PRINT "no changes"
      RETURN
*
*  Open FN's data (or its dictionary when FN ends .DICT) into RV; ROK = success.
   OPENREC:
      ROK = 0 ; LFN = LEN(FN)
      IF LFN > 5 AND FN[LFN-4, 5] = ".DICT" THEN
         OPEN "DICT", FN[1, LFN-5] TO RV THEN ROK = 1
      END ELSE
         OPEN FN TO RV THEN ROK = 1
      END
      RETURN
*
*  SAME = 1 when the live record LV matches the committed record CB.  A record
*  holding a NUL byte is binary (a compiled BP object): the CallC staging path
*  marshals with strlen and cannot carry it whole, so it never round-trips in
*  session — treat it as unreportable (SAME) rather than a perpetual change; such
*  objects belong in GITIGNORE and are rebuilt on the target by BUILD.  Otherwise
*  compare in git-space — marks (@AM) folded to newlines on BOTH sides — so a
*  record with a literal newline is not a false positive from GITCAT's
*  newline<->mark round-trip.
   SAMEREC:
      IF INDEX(LV, CHAR(0), 1) > 0 THEN SAME = 1 ; RETURN
      GLV = LV ; CONVERT AM TO LF IN GLV
      GCB = CB ; CONVERT AM TO LF IN GCB
      SAME = (GLV = GCB)
      RETURN
*
*  %FILE% control status: CHG = 1 when the live class differs from HEAD's.  The
*  modulo is a sticky default, so compare CLASS (DIR vs hash) only (mvx#8).
   CTLSTAT:
      CHG = 0
      R = CALLC GITCAT(REPO, P)
      OSREAD CB FROM REPO : "/gitcat" ELSE CB = ""
      CCL = TRIM(FIELD(CB, " ", 1))
      IF CCL[1,3] = "DIR" THEN CCL = "DIR" ELSE CCL = "hash"
      FILE = BASE ; GOSUB FILECLASS
      LCL = TRIM(FIELD(CTL, " ", 1))
      IF LCL[1,3] = "DIR" THEN LCL = "DIR" ELSE LCL = "hash"
      IF CCL # LCL THEN CHG = 1
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
