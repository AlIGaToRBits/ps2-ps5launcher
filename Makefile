EE_BIN  = launcher.elf
IRX_C   = iomanX_irx.c fileXio_irx.c usbd_irx.c bdm_irx.c bdmfs_fatfs_irx.c usbmass_bd_irx.c poweroff_irx.c audsrv_irx.c
EE_OBJS = main.o $(IRX_C:.c=.o)

EE_INCS    += -I$(GSKIT)/include -I$(PS2SDK)/ports/include
EE_LDFLAGS += -L$(GSKIT)/lib -L$(PS2SDK)/ports/lib
EE_LIBS    += -lgskit_toolkit -lgskit -ldmakit -lpng -lz -lpad -lpatches \
              -lelf-loader -lfileXio -lcdvd -lpoweroff -laudsrv -lmc -lm

all: $(EE_BIN)

%_irx.c: $(PS2SDK)/iop/irx/%.irx
	$(PS2SDK)/bin/bin2c $< $@ $*_irx

clean:
	rm -f $(EE_BIN) $(EE_OBJS) $(IRX_C)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
