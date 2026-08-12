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
CALL GIT.SENT(S)
* GIT TAG                     list tags
* GIT TAG name {commit}       lightweight tag at commit (HEAD if omitted)
* GIT TAG -a name -m message  annotated tag at HEAD
* GIT TAG -d name             delete a tag
A = FIELD(S, " ", 3)
OP = "list" ; NM = "" ; TGT = "" ; MSG = ""
BEGIN CASE
   CASE A = ""
      OP = "list"
   CASE A = "-d" OR A = "DELETE"
      OP = "delete" ; NM = FIELD(S, " ", 4)
   CASE A = "-a"
      OP = "add" ; NM = FIELD(S, " ", 4)
      * message is everything after "-m " (quotes trimmed)
      MP = INDEX(S, " -m ", 1)
      IF MP THEN
         MSG = TRIM(S[MP + 4, LEN(S) - MP - 3])
         IF MSG[1, 1] = '"' THEN MSG = MSG[2, LEN(MSG) - 2]
      END
   CASE 1
      OP = "add" ; NM = A ; TGT = FIELD(S, " ", 4)
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