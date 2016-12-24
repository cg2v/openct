#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif
#include <limits.h>
#include <openct/error.h>
#include <openct/logging.h>
#include <openct/device.h>
#include <openct/driver.h>
#include <openct/ifd.h>
#undef IFD_ERROR_NOT_SUPPORTED // we want the PCSC copy of this error code
#ifdef __APPLE__
#include <PCSC/wintypes.h>
#include <PCSC/pcsclite.h>
#include <PCSC/ifdhandler.h>
#else
#include <wintypes.h>
#include <pcsclite.h>
#include <ifdhandler.h>
#endif
/* Maximum number of readers handled */
#define IFDH_MAX_READERS        OPENCT_MAX_READERS

/* Maximum number of slots per reader handled */
#define IFDH_MAX_SLOTS          1

static int setup_done = 0;

typedef struct {
	BOOL connected;
	DWORD opens;
	DWORD reader_lun;
#ifdef HAVE_PTHREAD
	pthread_mutex_t mutex;
#endif
	ifd_reader_t *reader;
} IFDH_Context;

/* Matrix that stores conext information of all slots and readers */
static IFDH_Context *ifdh_context;

/* Mutexes for all readers */
#ifdef HAVE_PTHREAD
static pthread_mutex_t ifdh_contextlist_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static void __init() __attribute__((constructor));
static void __init() {
#ifdef HAVE_PTHREAD
	int i;
#endif
	ifd_config_parse(NULL);
	if (ifd_init()) {
		ct_error("Cannot initialize openct framework");
		return;
	}
	ifdh_context = calloc(IFDH_MAX_READERS, sizeof(IFDH_Context));
	if (ifdh_context == NULL) {
		ct_error("Cannot allocate context memory");
		return;
	}
#ifdef HAVE_PTHREAD
	for (i = 0 ; i < IFDH_MAX_READERS ; i++) {
		if (pthread_mutex_init(&ifdh_context[i].mutex, NULL)) {
			ct_error("Cannot initialize pthreads");
			return;
		}
	}
#endif
	setup_done = 1;
}

static IFDH_Context *getContextFor(DWORD Lun) {
	int i;
	DWORD readerLun = Lun >> 16;
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ifdh_contextlist_mutex); //check
#endif
	for (i = 0; i < IFDH_MAX_READERS; i++) {
		IFDH_Context *ctx = &ifdh_context[i];
		if (ctx->connected &&
		    readerLun == ctx->reader_lun) {
#ifdef HAVE_PTHREAD
			pthread_mutex_lock(&ctx->mutex); //check
#endif
			return ctx;
		}
#ifdef HAVE_PTHREAD
		pthread_mutex_unlock(&ifdh_contextlist_mutex); //check
#endif
	}
	return NULL;
}

static IFDH_Context *getFreeContext(void) {
	int i;
	IFDH_Context *ctx = NULL;
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ifdh_contextlist_mutex); //check
#endif
	for (i = 0; i < IFDH_MAX_READERS; i++) {
		ctx = &ifdh_context[i];
		if (!ctx->connected) {
#ifdef HAVE_PTHREAD
			pthread_mutex_lock(&ctx->mutex); //check
#endif
			break;
		}
	}
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ifdh_contextlist_mutex); //check
#endif
	if (i < IFDH_MAX_READERS)
		return ctx;
	return NULL;
}

static void unlockContext(IFDH_Context *ctx) {
#ifdef HAVE_PTHREAD
	pthread_mutex_lock(&ctx->mutex); //check
#endif
}

static void freeContext(IFDH_Context *ctx) {
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ctx->mutex); //check
#endif
#ifdef HAVE_PTHREAD
	pthread_mutex_lock(&ifdh_contextlist_mutex); //check
#endif
	ctx->connected = 0;
	ctx->reader_lun = 0;
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ifdh_contextlist_mutex); //check
#endif
}
RESPONSECODE 	IFDHCreateChannelByName (DWORD Lun, LPSTR DeviceName) {
	RESPONSECODE ret = IFD_SUCCESS;
#ifndef __APPLE__
	ifd_devid_t devid;
	char *devdup = NULL;
	char *p;
	char *devpath;
	const char *drivername;
	char *devicedesc;
#endif
	ifd_reader_t *reader;
        int interface_number = -1;
	IFDH_Context *ctx;
	DWORD slotLun = Lun & 0xFFFF;
	if (setup_done == 0)
		return IFD_COMMUNICATION_ERROR;

	/* Check to see if this reader is already open */
	ctx = getContextFor(Lun);

	/* found matching reader. Check if slot is valid, allow duplicate opens */
	if (ctx) {
		ret = IFD_SUCCESS;
		reader =  ctx->reader;
		if (slotLun > ctx->reader->nslots) {
			ct_error("Lun 0x%x specifies non-existant slot in reader %s", Lun, ctx->reader->name);
			ret = IFD_NO_SUCH_DEVICE;
		} else {
			ct_debug("Found channel %d of %s for lun 0x%x", slotLun, ctx->reader->name, Lun);
			ctx->opens++;
		}
#ifdef HAVE_PTHREAD
		pthread_mutex_unlock(&ctx->mutex); //check
#endif
		ct_debug("Reader for Lun 0x%x already open", Lun);
		return ret;
	}
	ct_debug("Open %s for lun %x", DeviceName, Lun);
	ctx = getFreeContext();
	if (ctx == NULL) {
		ct_error("No available context slots for %s", DeviceName);
		return IFD_COMMUNICATION_ERROR;
	}
#ifdef __APPLE__
	unlockContext(ctx);
	return IFD_NO_SUCH_DEVICE; /* no scanning yet */
#else
	/* format: usb:%04x/%04x, vendor, product */
	if (strncmp("usb:", DeviceName, 4) != 0)
	{
		ct_error("device name does not start with \"usb:\": %s",
			 DeviceName);
		ret = IFD_NO_SUCH_DEVICE;
		goto out;
	}
	devdup = strdup(DeviceName);
	if (devdup == NULL) {
		ret = IFD_COMMUNICATION_ERROR;
		goto out;
	}
	p = strchr(devdup, ':'); // skip usb:, should always succeed, given comparison above
	p = strchr(p+1, ':'); // skip device name
	if (p == NULL) {
	noscan:
		ct_error("device scanning not implemented; need udev info, not just '%s'", DeviceName);
		ret = IFD_NO_SUCH_DEVICE;
		goto out;
	}
	*p++ = 0;
	devpath = p;

	/* format usb:%04x/%04x:libudev:%d:%s
	 * with %d set to
	 * 01 (or whatever the interface number is)
	 * and %s set to
	 * /dev/bus/usb/008/004
	 */
	if (!strncmp(devpath, "libudev:", 8)) {
		/* convert the interface number */
		interface_number = atoi(devpath + 8 /* "libudev:" */);
		ct_debug("interface_number: %d", interface_number);
		devpath = strchr(devpath+8, ':');
		if (devpath)
			devpath++;
		else
			goto noscan;
	} else {
		goto noscan;
	}
	if (ifd_device_id_parse(devdup, &devid) < 0) {
		ct_error("device id %s in '%s' not understood", devdup, DeviceName);
		ret = IFD_NO_SUCH_DEVICE;
		goto out;
	}
	if (!(drivername = ifd_driver_for_id(&devid))) {
		ct_error("device id %s has no driver", devdup);
		ret = IFD_NO_SUCH_DEVICE;
		goto out;
	}
	devicedesc = malloc(strlen(devpath) +5);
	if (devicedesc == NULL) {
		ct_error("malloc failure");
		ret = IFD_COMMUNICATION_ERROR;
		goto out;
	}
	sprintf(devicedesc, "usb:%s", devpath);
	reader = ifd_open(drivername, devicedesc);
	free(devicedesc);
	if (reader == NULL) {
		ret = IFD_COMMUNICATION_ERROR;
		goto out;
	}
	ct_debug("Device %s is %s for lun 0x%x", DeviceName, reader->name, Lun);
	if (slotLun > reader->nslots) {
		ct_error("Lun 0x%x specifies non-existant slot in reader %s", Lun, reader->name);
		ifd_close(reader);
		ret = IFD_NO_SUCH_DEVICE;
		goto out;
	}
	ctx->reader_lun = Lun >> 16;
	ctx->reader = reader;
	ctx->connected = 1;
	ctx->opens = 1;
out:
#ifdef HAVE_PTHREAD
	pthread_mutex_unlock(&ctx->mutex); //check
#endif
	return ret;
#endif
}

RESPONSECODE IFDHCreateChannel (DWORD Lun, DWORD Channel) {
	return IFD_NO_SUCH_DEVICE;
}
RESPONSECODE IFDHCloseChannel (DWORD Lun) {
	IFDH_Context *ctx;
	DWORD slotLun = Lun & 0xFFFF;
	RESPONSECODE ret = IFD_SUCCESS;

	if (setup_done == 0)
		return IFD_COMMUNICATION_ERROR;
	/* Check to see if this reader is open*/
	ctx = getContextFor(Lun);
	if (ctx == NULL)
		return IFD_NO_SUCH_DEVICE;
	if (slotLun > ctx->reader->nslots) {
		ct_error("Lun 0x%x specifies non-existant slot in reader %s", Lun, ctx->reader->name);
		ret = IFD_NO_SUCH_DEVICE;
		goto out;
	}
	ct_debug("Close lun %x", Lun);
	if (ctx->opens-- > 1)
		goto out;
	ifd_deactivate(ctx->reader);
	ifd_close(ctx->reader);
	ctx->reader = NULL;
	freeContext(ctx);
	return IFD_SUCCESS;
out:
	unlockContext(ctx);
	return ret;
}
RESPONSECODE IFDHControl (DWORD Lun, DWORD dwControlCode, PUCHAR TxBuffer, DWORD TxLength,
			  PUCHAR RxBuffer, DWORD RxLength, LPDWORD pdwBytesReturned) {
	return IFD_SUCCESS;
}

RESPONSECODE IFDHGetCapabilities(DWORD Lun, DWORD Tag, PDWORD Length,
				 PUCHAR Value) {
	IFDH_Context *ctx;
	DWORD slotLun = Lun & 0xFFFF;
	void *outdata = NULL;
	size_t outlen;
	unsigned char charvalue;
	RESPONSECODE ret = IFD_SUCCESS;

	if (setup_done == 0)
		return IFD_COMMUNICATION_ERROR;
	ct_debug("GetCap lun 0x%x Tag 0x%x", Lun, Tag);
	switch (Tag) {
	case TAG_IFD_SIMULTANEOUS_ACCESS:
		charvalue = IFDH_MAX_READERS;
		outdata = &charvalue;
		outlen = 1;
		break;
	case TAG_IFD_THREAD_SAFE:
	case TAG_IFD_SLOT_THREAD_SAFE:
	case TAG_IFD_POLLING_THREAD_KILLABLE:
#ifdef HAVE_PTHREAD
		charvalue = 1;
#else
		charvalue = 0;
#endif
		outdata = &charvalue;
		outlen = 1;
		break;
	case TAG_IFD_POLLING_THREAD_WITH_TIMEOUT:
		/* ifd_event() tries to update the ct status file, so
		   we can't call it. Most supported openct readers other than CCID
 		   (which will not be used ) are tokens whose "card" is always present */
#if 0
#ifdef HAVE_PTHREAD
		outdata = &IFDH_Poll;
#else
		outdata = NULL;
#endif
#else
		outdata = NULL;
#endif
		outlen = sizeof(void *);
		break;
	default:
		break;
	}
	if (outdata == NULL) { /* tags that need a reader */
		/* Check to see if this reader is open*/
		ctx = getContextFor(Lun);
		if (ctx == NULL)
			return IFD_NO_SUCH_DEVICE;
		if (slotLun > ctx->reader->nslots) {
			ct_error("Lun 0x%x specifies non-existant slot in reader %s", Lun, ctx->reader->name);
			ret = IFD_NO_SUCH_DEVICE;
			goto out;
		}
		switch (Tag) {
		case TAG_IFD_ATR:
			memset(Value, 0, *Length); /* clear in case atr is empty */
			outdata = ctx->reader->slot[slotLun].atr;
			outlen = ctx->reader->slot[slotLun].atr_len;
			break;
		case TAG_IFD_SLOTNUM:
			charvalue = slotLun;
			outdata = &charvalue;
			outlen = 1;
			break;
		default:
			ret = IFD_ERROR_TAG;
			goto out;
		}
	}
	if (outdata != NULL) {
		if (outlen > *Length) {
			ret = IFD_ERROR_INSUFFICIENT_BUFFER;
			goto out;
		}
		memcpy(Value, outdata, outlen);
		*Length = outlen;
	}
out:
	unlockContext(ctx);
	return ret;
}

RESPONSECODE IFDHICCPresence(DWORD Lun) {
	IFDH_Context *ctx;
	DWORD slotLun = Lun & 0xFFFF;
	RESPONSECODE ret = IFD_SUCCESS;
	int status;

	if (setup_done == 0)
		return IFD_COMMUNICATION_ERROR;
	/* Check to see if this reader is open*/
	ctx = getContextFor(Lun);
	if (ctx == NULL)
		return IFD_NO_SUCH_DEVICE;
	if (slotLun > ctx->reader->nslots) {
		ct_error("Lun 0x%x specifies non-existant slot in reader %s", Lun, ctx->reader->name);
		ret = IFD_NO_SUCH_DEVICE;
		goto out;
	}
	if (ifd_card_status(ctx->reader, slotLun, &status) < 0) {
		ret = IFD_COMMUNICATION_ERROR;
		goto out;
	}
	if ((status & IFD_CARD_PRESENT) == 0)
		ret = IFD_ICC_NOT_PRESENT;
	//ct_debug("Presence lun 0x%x %d", Lun, ret);
out:
	unlockContext(ctx);
	return ret;
}

RESPONSECODE IFDHPowerICC(DWORD Lun, DWORD Action, PUCHAR Atr,
			  PDWORD AtrLength) {
	IFDH_Context *ctx;
	DWORD slotLun = Lun & 0xFFFF;
	int erc;
	RESPONSECODE ret = IFD_SUCCESS;

	if (setup_done == 0)
		return IFD_COMMUNICATION_ERROR;
	/* Check to see if this reader is open*/
	ctx = getContextFor(Lun);
	if (ctx == NULL)
		return IFD_NO_SUCH_DEVICE;
	if (slotLun > ctx->reader->nslots) {
		ct_error("Lun 0x%x specifies non-existant slot in reader %s", Lun, ctx->reader->name);
		ret = IFD_NO_SUCH_DEVICE;
		goto out;
	}
	switch(Action) {
	case IFD_POWER_UP:
		erc = ifd_card_request(ctx->reader, slotLun, 0, NULL, NULL, 0);
		break;
	case IFD_RESET:
		erc = ifd_card_reset(ctx->reader, slotLun, NULL, 0);
	case IFD_POWER_DOWN:
		erc = ifd_card_eject(ctx->reader, slotLun, 0, NULL);
		break;
	default:
		ret = IFD_NOT_SUPPORTED;
		goto out;
	}
	ct_debug("Power lun %d %d %d (%d)", Lun, Action, erc, *AtrLength);
	if (erc < 0) {
		ret = IFD_COMMUNICATION_ERROR;
		goto out;
	}
	if (Action != IFD_POWER_DOWN) {
		if (erc > *AtrLength) {
			ret = IFD_ERROR_INSUFFICIENT_BUFFER;
			goto out;
		}
		memcpy(Atr, ctx->reader->slot[slotLun].atr, erc);
		*AtrLength = erc;
	}
out:
	unlockContext(ctx);
	return ret;
}

RESPONSECODE IFDHSetCapabilities(DWORD Lun, DWORD Tag, DWORD Length,
				 PUCHAR Value) {
	return IFD_SUCCESS;
}

/* openct drivers handle this internally */
RESPONSECODE IFDHSetProtocolParameters (DWORD Lun, DWORD Protocol, UCHAR Flags,
					UCHAR PTS1, UCHAR PTS2, UCHAR PTS3) {
	ct_debug("SetProtocol lun 0x%x %d 0x%x", Lun, Protocol, Flags);
	if (Protocol > 1)
		return IFD_NOT_SUPPORTED;
	if (Flags)
		return IFD_ERROR_PTS_FAILURE;
	return IFD_SUCCESS;
}


RESPONSECODE IFDHTransmitToICC (DWORD Lun, SCARD_IO_HEADER SendPci, PUCHAR TxBuffer,
				DWORD TxLength, PUCHAR RxBuffer, PDWORD RxLength,
				PSCARD_IO_HEADER RecvPci) {
	IFDH_Context *ctx;
	DWORD slotLun = Lun & 0xFFFF;
	int erc;
	RESPONSECODE ret = IFD_SUCCESS;

	if (setup_done == 0)
		return IFD_COMMUNICATION_ERROR;
	/* Check to see if this reader is open*/
	ctx = getContextFor(Lun);
	if (ctx == NULL)
		return IFD_NO_SUCH_DEVICE;
	if (slotLun > ctx->reader->nslots) {
		ct_error("Lun 0x%x specifies non-existant slot in reader %s", Lun, ctx->reader->name);
		ret = IFD_NO_SUCH_DEVICE;
		goto out;
	}

	/* protocol selection is transparent. Assume that the driver picked whichever of T=0 or T=1
	   is correct. Don't allow other protocols */
	if (SendPci.Protocol > 1) {
		ret = IFD_NOT_SUPPORTED;
		goto out;
	}
	erc = ifd_card_command(ctx->reader, slotLun, TxBuffer, TxLength, RxBuffer,
			       *RxLength);
	ct_debug("Transmit lun 0x%x %d %d", Lun, TxLength, erc);
	if (erc < 0) {
		ret = IFD_COMMUNICATION_ERROR;
		if (erc == IFD_ERROR_BUFFER_TOO_SMALL)
			ret = IFD_ERROR_INSUFFICIENT_BUFFER;
		goto out;
	}
	*RxLength = erc;
	if  (RecvPci)
		RecvPci->Protocol = SendPci.Protocol;
out:
	unlockContext(ctx);
	return ret;
}
