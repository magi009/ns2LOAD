"""Which of the two tools this copy is.

The same source makes both: nx-relink, for Switch games on a Switch, and nx2-relink, for Switch 2 games on a Switch 2. Each is
published with the options of its own console alone (a Switch game can't yet be taken to a Switch 2, so neither offers it), and
this one word is what tells them apart: the options the command line and the window show, the kind of game they take, the folder
of the card a port's files go in.

    'switch'    nx-relink     a Switch game's NSP           ->  /nx-relink/<name>/
    'switch2'   nx2-relink    a Switch 2 game's NUP/folder  ->  /nx2-relink/<name>/
    'dev'       both, with what is still being worked on (the copy the two are made from)
"""
EDITION = 'switch2'
TOOL = {'switch': 'nx-relink', 'switch2': 'nx2-relink', 'dev': 'nx2-recomp'}[EDITION]
VERSION = '0.1.12'
