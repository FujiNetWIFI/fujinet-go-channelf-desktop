# The app's stand-in BIOS (core/chf/stub.c, built from stub.asm by
# tools/stub/build-stub.sh) against the contract observed on the real one
# (bios_contract.h, made by tools/stub/observe.py --header): RESET, clear screen,
# push K and pop K, tier 1 held and tier 2 reported, the cartridge handshake, and a
# machine on the stand-in booting the FujiNet cartridge's CONFIG. No ROM, no MAME.
chf_test(bios_stub_test bios_stub_test.c)
add_test(NAME bios_stub COMMAND bios_stub_test)
set_tests_properties(bios_stub PROPERTIES TIMEOUT 120)
