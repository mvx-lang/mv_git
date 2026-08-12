* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* /**
*  * @file GIT.TAG
*  * @version 1.0
*  */
SUBROUTINE GIT.TAG
* GIT TAG                     list tags
* GIT TAG name {commit}       lightweight tag at commit (HEAD if omitted)
* GIT TAG -a name -m message  annotated tag at HEAD
* GIT TAG -d name             delete a tag
* Options are decoded by the cmd dispatch (GIT.CMD.FLAG a/m/d); read them here.
CALL GIT.CMD.HAS("a", HASA) ; CALL GIT.CMD.VAL("a", ANM)
CALL GIT.CMD.HAS("d", HASD) ; CALL GIT.CMD.VAL("d", DNM)
CALL GIT.CMD.VAL("m", MSG)
CALL GIT.CMD.ARG(1, ARG1) ; CALL GIT.CMD.ARG(2, ARG2)
OP = "list" ; NM = "" ; TGT = ""
BEGIN CASE
   CASE HASD
      OP = "delete" ; NM = DNM
   CASE HASA
      OP = "add" ; NM = ANM
   CASE ARG1 # ""
      OP = "add" ; NM = ARG1 ; TGT = ARG2
END CASE
IF OP # "list" AND NM = "" THEN
   PRINT "usage: GIT TAG {name {commit} | -a name -m msg | -d name}"
   RETURN
END
$IFDEF MVX
OUT = ""
CALL GITTAG(".git", OP, NM, TGT, MSG, OUT)
FOR I = 1 TO DCOUNT(OUT, @AM)
   PRINT OUT<I>
NEXT I
$ELSE
REPO = @PATH : "/.git"
R = CALLC GITTAG(REPO, OP, NM, TGT, MSG)
CALL GITUDT.SHOW(REPO)
$ENDIF
RETURN