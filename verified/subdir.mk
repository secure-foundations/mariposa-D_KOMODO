DAFNYFLAGS = /noNLarith /timeLimit:5 /trace
# NB: include paths are relative to the (generated) dfy file, not the CWD
SPARTAN_INCLUDES = -i ARMspartan.dfy -i ARMprint.dfy #$(SHA256_INCLUDES)
KEVLAR_INCLUDES  = -i kev_constants.dfy
SHA256_INCLUDES = -i sha_common.s.dfy -i sha256.s.dfy -i sha256.i.dfy 
SPARTAN_DEPS = $(dir)/ARMdef.dll $(dir)/ARMprint.dll $(dir)/ARMspartan.dll
KEVLAR_DEPS  = $(dir)/kev_constants.dll

# temp target to build top-level verified stuff
verified: $(dir)/ARMtest1.o

%.dfy: %.sdfy $(dir)/ARMdecls.sdfy $(SPARTAN_DEPS)
	$(SPARTAN) $(dir)/ARMdecls.sdfy $< -out $@ $(SPARTAN_INCLUDES) $(KEVLAR_INCLUDES)

%.exe: %.dfy
	$(DAFNY) $(DAFNYFLAGS) $<

%.S: %.exe
	$< > $@

# These DLL files are not consumed by anything, but listing them as
# dependencies (and generating them) forces Dafny to verify the
# relevant modules
%.dll: %.dfy
	$(DAFNY) $(DAFNYFLAGS) $< /out:$*

CLEAN := $(CLEAN) $(dir)/*.exe $(dir)/*.dll $(dir)/*.pdb $(dir)/*.S $(dir)/*.o

# manual deps for Dafny code
$(dir)/ARMdef.dll: $(dir)/assembly.s.dfy
$(dir)/ARMprint.dll: $(dir)/ARMdef.dfy
$(dir)/ARMspartan.dll: $(dir)/ARMdef.dfy

# keep these "intermediate" files around, to avoid pointless re-verification
.SECONDARY: $(SPARTAN_DEPS)

# temp target to produce a bootable image
$(dir)/%.img: $(dir)/%.o
	$(OBJCOPY) $< -O binary $@
