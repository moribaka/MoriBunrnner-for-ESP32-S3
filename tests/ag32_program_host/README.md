# AG32 lifecycle tests

Compile and run from the repository root with a host GCC:

```powershell
gcc -std=gnu11 -O2 -Wall -Wextra -I tests/ag32_program_host/stubs -I main tests/ag32_program_host/test_program.c main/ag32_batch_format.c -o build/test_ag32_program.exe
./build/test_ag32_program.exe example/moriburnner_ag32_batch.bin
gcc -std=gnu11 -O2 -Wall -Wextra -I tests/ag32_program_host/stubs tests/ag32_program_host/test_spi_swd.c -o build/test_spi_swd.exe
./build/test_spi_swd.exe
```

The programmer test compiles the actual implementation and injects failures at
allocation, file open, SWD acquisition/connection, halt, ID read/validation,
unlock, erase, program, verify, reset, resume and SPI restoration. It checks
first-error preservation, cleanup error separation, recovery lockout, normal
completion and an immutable source snapshot. Admission checks cover a single
statically allocated worker, USB ownership, cartridge startup and duplicate jobs.

The SPI test compiles the production SWD ownership transition include with GPIO
failure injection. It checks lock balance, rollback and recovery behavior.

MinGW's test-only `fmemopen` substitute uses an immutable temporary stream;
firmware uses the ESP libc implementation. These tests validate control flow,
not electrical SWD timing, NOR physics or task scheduling.

Device regression is `tests/ui_device/test_ag32_update.py`. Without `--program`
it checks read-only SWD connections and a missing-file startup failure. With
`--program` it writes the explicitly selected, SHA256-checked batch, validates
every terminal field, exercises UI confirmation/navigation and checks that
other endpoints reject concurrent operations. A failed update stops the test;
it is never retried. `--native-only` runs all requested cycles through the UI.
