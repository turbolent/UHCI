# OPENSTEP 4.2 DriverKit bundle, modelled after DriverKit_Examples.
NAME = UHCI
PROJECTVERSION = 1.1
LANGUAGE = English
LOCAL_RESOURCES = Localizable.strings
GLOBAL_RESOURCES = Default.table README.md
TOOLS = UHCI_reloc.tproj
OTHERSRCS = Makefile.preamble Makefile Makefile.postamble
MAKEFILEDIR = /NextDeveloper/Makefiles/app
MAKEFILE = bundle.make
SOURCEMODE = 444
BUNDLE_EXTENSION = config
-include Makefile.preamble
include $(MAKEFILEDIR)/$(MAKEFILE)
-include Makefile.postamble
-include Makefile.dependencies
