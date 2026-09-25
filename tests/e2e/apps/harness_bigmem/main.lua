-- Harness fixture: never runs; app.json asks for more contiguous PSRAM
-- than the heap has, so the launcher refuses it.
picocalc.sys.log("H:BIGMEM_RAN")
