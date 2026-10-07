NAME = UHCI_reloc
PROJECTVERSION = 1.1
LANGUAGE = English
CLASSES = UHCIController.m UHCIStorage.m
HFILES = UHCIController.h UHCICore.h UHCIRegs.h UHCITypes.h UHCIMemory.h UHCIUSBNames.h UHCIInterruptState.h UHCIPCI.h UHCIVersion.h USBMassStorage.h USBStorageSCSI.h USBStorageBuffer.h USBStorageWait.h UHCIStorageStats.h USBCore.h UHCICompletionWait.h
CFILES = UHCICore.c UHCIInterruptState.c UHCIPCI.c USBCore.c USBMassStorage.c USBStorageSCSI.c USBStorageBuffer.c USBStorageWait.c UHCICompletionWait.c
OTHERSRCS = UHCIEnumeration.inc Makefile.preamble Makefile Makefile.postamble Load_Commands.sect
MAKEFILEDIR = /NextDeveloper/Makefiles/app
MAKEFILE = tool.make
SOURCEMODE = 444
LIBS =
DEBUG_LIBS = $(LIBS)
PROF_LIBS = $(LIBS)
-include Makefile.preamble
include $(MAKEFILEDIR)/$(MAKEFILE)
-include Makefile.postamble
-include Makefile.dependencies
