-- Harness fixture: leave through sys.exit (the exit sentinel).
picocalc.sys.log("H:EXIT_START")
picocalc.sys.exit()
picocalc.sys.log("H:NOT_REACHED")
