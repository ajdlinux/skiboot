SUBDIRS += $(PLATDIR)/simics

SIMICS_OBJS = simics.o
SIMICS = $(PLATDIR)/simics/built-in.o
$(SIMICS): $(SIMICS_OBJS:%=$(PLATDIR)/simics/%)